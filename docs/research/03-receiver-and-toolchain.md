# 03 — The Phone Receiver and the Build/Test Toolchain

Research note for **PhonePostMix** (DAW master bus → phone, live, over Wi-Fi).
Date of research: **2026-08-27**. All version claims verified against sources linked inline.

Scope: (1) what the phone side should be, (2) what the web platform can actually do for
live low-latency audio in 2026, (3) how to build the JUCE plugin so strangers can clone
and compile it, (4) how to test a plugin with no commercial DAW, (5) repo/doc conventions.

**Bottom line up front:**

1. Ship **a plain URL opened in mobile Safari/Chrome** as the default and only v1 receiver.
   Nothing else survives the "someone clones this repo on a Tuesday and it works" test.
2. Feed it with **WebRTC** (`RTCPeerConnection`, Opus), not WebSocket+AudioWorklet, unless
   you specifically need bit-exact PCM. WebRTC gets you jitter buffering, packet loss
   concealment, and background-thread decode for free, and it is the one path that is
   fully supported on iOS.
3. Accept that the Bluetooth link, not your code, is the latency budget. AirPods add
   80–220 ms. Wired/USB-C headphones on the phone are the only configuration where
   sub-100 ms end-to-end is even arguable.
4. Build with **CMake + JUCE via `FetchContent`** (JUCE 9.0.1, Aug 2026), one
   `CMakeLists.txt`, GitHub Actions matrix over macOS/Windows/Linux, `pluginval` +
   `auval` as the CI gate, Catch2 for DSP unit tests. Do not require the Projucer.

---

## Part 1 — Receiver options compared

### 1.0 The honest latency budget

Before comparing app frameworks, note where the milliseconds actually go for a
"DAW → Wi-Fi → phone" monitor path:

| Stage | Typical cost |
|---|---|
| Plugin capture + encode buffer | 5–20 ms (your choice of block size) |
| Wi-Fi LAN hop (good 5 GHz AP) | 2–10 ms, spiky on 2.4 GHz / congested APs |
| Receiver jitter buffer | 20–60 ms (WebRTC NetEq adapts; this is the safety margin) |
| Browser/OS output buffer | 10–40 ms mobile |
| **Wired headphones out of the phone** | ~0 ms |
| **AirPods Pro 2/3 (AAC, H2 chip)** | **80–160 ms** |
| **AirPods 3rd gen** | **150–220 ms** |
| **AirPods Max** | **100–180 ms** |
| **Android + AAC/SBC** | **200–280 ms** |

Sources: [onlineaudiotest.com AirPods Pro 2 latency guide](https://onlineaudiotest.com/devices/airpods-pro-2/),
[AirPods Max](https://onlineaudiotest.com/devices/airpods-max/),
[AirPods latency test](https://onlineaudiotest.com/airpods-latency-test/).

**Implication:** the choice between "web page" and "native Swift app" moves the total by
maybe 20–40 ms. The choice between "AirPods" and "wired" moves it by 150 ms. Optimising
the framework before you have told the user to plug in a cable is optimising the wrong
term. A native app cannot fix A2DP. Nothing can — A2DP is designed for playback buffering,
not real-time monitoring
([Android low-latency audio docs](https://source.android.com/docs/core/audio/latency/app)).

This is the single strongest argument for the URL: the ceiling imposed by the transport
and the headphones is so much larger than the framework delta that paying App Store tax
to recover 30 ms is a bad trade.

### 1.1 Comparison table

| | (a) Plain web page (URL) | (b) PWA (installed) | (c) React Native / Flutter | (d) Native Swift / Kotlin |
|---|---|---|---|---|
| **Playback latency** | WebRTC/Opus decode + browser output buffer. Realistically **30–100 ms** on mobile on top of network. AudioWorklet path can reach ~128-frame quantum but the OS output buffer dominates. | Identical to (a) — same WebKit/Blink engine. Installing changes nothing about audio. | Bridge to native audio; only as good as the plugin you use. Flutter has **no good low-latency playback plugin on iOS** ([LogRocket](https://blog.logrocket.com/best-flutter-music-streaming-options/)); RN needs a custom Oboe/AVAudioEngine native module ([example](https://suyashsingh.in/blog/android-oboe-with-rust-and-react-native)). | Best possible: `AVAudioEngine` + `AVAudioSession` `.setPreferredIOBufferDuration` on iOS; Oboe `PerformanceMode::LowLatency` + `SharingMode::Exclusive` on Android (~20 ms round trip on good devices). |
| **Background audio (screen off / app switched)** | **No on iOS.** Web Audio and WebRTC are suspended as soon as Safari backgrounds or the screen locks. `<audio>`/`<video>` element playback survives; Web Audio does not. Android Chrome is more permissive but still throttles. | **Same as (a) on iOS.** Installing to home screen does not grant background audio. iOS has no Background Audio entitlement for web apps. | Yes, via the host app's `UIBackgroundModes: audio` — but you must write the native audio module anyway. | **Yes** — `UIBackgroundModes: audio` + `AVAudioSession .playback`; Android foreground service. Only option with real background audio. |
| **Keep screen on** | **Yes.** `navigator.wakeLock.request('screen')` — Safari iOS **16.4+**, Chrome/Firefox everywhere; >94% global support as of 2026 ([web.dev](https://web.dev/blog/screen-wake-lock-supported-in-all-browsers), [caniuse](https://caniuse.com/wake-lock)). **Requires a secure context** (HTTPS or `localhost`). | Yes, but note Wake Lock was **broken in installed iOS PWAs until iOS 18.4** ([WebKit bug 254545](https://bugs.webkit.org/show_bug.cgi?id=254545)). Plain Safari was always fine. | Yes (`KeepAwake` / `FLAG_KEEP_SCREEN_ON`). | Yes (`UIApplication.isIdleTimerDisabled`). |
| **Bluetooth / AirPods** | Routing is entirely the OS's business; the browser follows the system output. `AudioContext.setSinkId()` is **not supported on Safari/iOS**, so you cannot pick the output device from the page. Latency as per §1.0. | Same. | Same as native if you write the module. | Can set `AVAudioSession` category/mode (e.g. `.playAndRecord`, `.voiceChat`) which forces the **HFP/low-latency** path — lower latency at the cost of mono-ish, narrowband-ish quality. This is the only genuine BT advantage of native, and for monitoring a music mix it's usually not worth the quality hit. |
| **Live stream reliability** | **Good, via WebRTC.** Opus is a mandatory WebRTC codec (RFC 7874); NetEq jitter buffer + Opus FEC/PLC degrade gracefully. WebSocket-over-TCP retransmits instead, adding **100+ ms spikes** on a bad Wi-Fi link ([getstream.io](https://getstream.io/blog/webrtc-websocket-av-sync/), [nanocosmos](https://www.nanocosmos.net/blog/webrtc-latency/)). | Same. | Same, if you use a WebRTC lib. | Same, plus you may use raw UDP/RTP with your own concealment. |
| **Distribution friction** | **Zero.** A URL. QR code on the plugin GUI. No account, no store, no signing, no review, no install, no update mechanism (the page *is* the update). Works on iPhone, Android, iPad, a second laptop, a friend's phone. | Low but non-zero: user must manually "Add to Home Screen" — iOS has **no install prompt**, so you must draw instructions ([MagicBell](https://www.magicbell.com/blog/pwa-ios-limitations-safari-support-complete-guide)). Many users never do it. | **High.** App Store review, $99/yr Apple Developer Program, TestFlight for betas: 100 internal testers (instant) / 10,000 external (Beta App Review, ~24 h per version), or Ad Hoc capped at **100 devices** ([Apple](https://developer.apple.com/testflight/), [tester limits](https://techconcepts.org/blog/testflight-guide)). Plus RN/Flutter toolchain in the repo that contributors must install. | **Highest.** Everything in (c) plus two separate codebases. |
| **Contributor cost (repo clonability)** | A static `index.html`. Any HTTP server. No SDK. | Same + a manifest and service worker. | Node + Xcode + Android Studio + CocoaPods/Gradle, or Flutter SDK. | Xcode + Android Studio + two languages. |

### 1.2 Strong evaluation of "just open a URL" as the default

**Where it wins, decisively:**

- **Distribution is the whole product.** PhonePostMix's value is "I want to hear my mix on
  my phone *right now*". Any step between "click Start in the plugin" and "sound in ear"
  destroys that. A QR code rendered by the JUCE editor → camera → Safari opens → tap
  "Listen" → audio. That is four seconds. TestFlight is four *days*.
- **Cross-platform for free.** iPhone, Android, iPad, a spare laptop, a smart TV browser.
  One implementation.
- **No Apple relationship required.** No $99/yr, no review, no risk of rejection for
  "app is not sufficiently different from a website" (App Review Guideline 4.2 —
  a thin streaming client is exactly the kind of app that gets that rejection).
- **Updates are instant and atomic.** Ship a new build of the plugin, the page it serves
  updates with it. No version skew between plugin and receiver — which is a real hazard
  for a bespoke wire protocol.
- **Contributors can hack the receiver in a text editor.** This matters enormously for a
  clone-and-run repo.

**Where it genuinely hurts, and how to mitigate:**

| Problem | Mitigation |
|---|---|
| **iOS suspends Web Audio when the screen locks or Safari backgrounds.** This is the big one. | Hold a **screen wake lock** and tell the user to leave the page in the foreground. Frame the product as "phone sits on the desk / in your hand while you monitor" — which is the actual use case. Also request `navigator.audioSession.type = 'playback'` (§2.6). If a user must monitor with the phone in a pocket, that is the one feature that legitimately requires a native app; document it as a known limitation rather than building a native app for it. |
| **The silent/ringer switch mutes Web Audio (but not `<audio>` elements)** on iOS. | Set `navigator.audioSession.type = 'playback'` (Safari-only, shipped), and fall back to the classic looping-silent-`<audio>` trick (`unmute-ios-audio`). See §2.6. |
| **A LAN IP over plain HTTP is not a secure context** — no Wake Lock, no service worker, no `SharedArrayBuffer`, no `getUserMedia`. Only `http://localhost` is exempt ([MDN Secure Contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Defenses/Secure_Contexts)). | This is the #1 practical trap for "plugin serves a page on `http://192.168.1.42:8080`". Options, in order of preference: (a) serve HTTPS from the plugin with a self-signed cert and have the user accept it once — ugly; (b) use a `*.localhost`-style trusted name; (c) use a public relay/tunnel with a real cert; (d) **just don't need a secure context**: WebRTC's `RTCPeerConnection` (receive-only, no mic) and `AudioContext` both work on insecure origins — you lose Wake Lock and service workers but keep the audio. Verify this on your target iOS version before committing. Practically: **plan for HTTPS**, because Wake Lock is close to essential here. |
| **Autoplay:** audio cannot start without a user gesture. | Design for it: the page's only control is a giant **Listen** button. This is not a workaround, it's good UX. |
| No home-screen icon / no push. | Irrelevant for this product. |

**Recommendation:** URL-only for v1. Add a **PWA manifest** later as a pure cosmetic
upgrade (home-screen icon, standalone chrome) — it costs ~20 lines and changes no
capability. Revisit native **only** if background-with-screen-off monitoring turns out to
be a real user demand, and even then ship it as a separate optional repo.

---

## Part 2 — Web Audio for live low-latency playback, verified for 2026

### 2.1 AudioWorklet

- **Supported on iOS Safari since 14.5** (26 April 2021) and macOS Safari 14.1;
  caniuse shows continuous support through **iOS Safari 26.3**
  ([caniuse mdn-api_audioworklet](https://caniuse.com/mdn-api_audioworklet),
  [caniuse wf-audio-worklet](https://caniuse.com/wf-audio-worklet)).
  Claims that "AudioWorklet doesn't work on iOS" are stale — they date from 2019–2020.
- Render quantum is 128 frames (~2.9 ms @ 44.1 kHz). That is the *worklet* granularity;
  the *device* output buffer is separate and larger.
- Realistic figures: **5–20 ms on desktop, 30–100 ms on mobile**
  ([StageAlly, 2026](https://stageally.com/articles/best-browser-for-web-audio)).
- `AudioContext.setSinkId()` is **not** supported on Safari/iOS — no output-device picking.

```js
const ctx = new AudioContext({ latencyHint: 'interactive', sampleRate: 48000 });
await ctx.audioWorklet.addModule('/worklet.js');   // must be a separate file, same origin
const node = new AudioWorkletNode(ctx, 'jitter-buffer-player', {
  numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
});
node.connect(ctx.destination);
```

`latencyHint` is a *request*, not a guarantee — read back
[`ctx.baseLatency`](https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/baseLatency)
and [`ctx.outputLatency`](https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/outputLatency)
after construction. Caveat: `outputLatency` has patchy implementation — treat both as
diagnostics to display, not as numbers to build sync logic on.

### 2.2 SharedArrayBuffer and the COOP/COEP tax

`SharedArrayBuffer` is the standard way to feed an AudioWorklet without `postMessage`
latency — a wait-free SPSC ring buffer written by the main thread / a Worker and read by
the worklet ([ringbuf.js](https://github.com/padenot/ringbuf.js)).

It requires **cross-origin isolation**:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

and a **secure context**. Verify at runtime with `self.crossOriginIsolated === true`.
Safari has supported `SharedArrayBuffer` since **15.2** under isolation
([MDN](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer)).

Two historical WebKit bugs are worth knowing about because they still poison a lot of
StackOverflow advice:

- [WebKit 237144](https://bugs.webkit.org/show_bug.cgi?id=237144) — SAB passed via
  `AudioWorkletNode` `processorOptions` was not actually shared. **RESOLVED FIXED**,
  r290531, 25 Feb 2022. Long since shipped.
- [WebKit 220038](https://bugs.webkit.org/show_bug.cgi?id=220038) — sending a SAB by
  `port.postMessage()` to a worklet. Chris Dumez noted in the 237144 thread that this
  path remained broken separately. **Practical rule: pass the SharedArrayBuffer in
  `processorOptions` at construction time, not over `port.postMessage`.**

If your page is served from static hosting where you can't set headers (GitHub Pages), use
[`coi-serviceworker`](https://github.com/gzuidhof/coi-serviceworker) — a service worker
that injects COOP/COEP; it reloads once on first visit. It must be its own file on your
own origin, and the page must still be HTTPS or localhost.
See also [tomayac's writeup](https://blog.tomayac.com/2025/03/08/setting-coop-coep-headers-on-static-hosting-like-github-pages/).

**Judgement for PhonePostMix:** COOP/COEP + HTTPS + a service worker is a lot of
infrastructure to bolt onto a page served by a C++ plugin over a LAN. If you go the WebRTC
route you **do not need SharedArrayBuffer at all** — the browser's own decoder and jitter
buffer run off-main-thread. Keep SAB in reserve for a "raw PCM, bit-exact, desktop-only"
mode.

### 2.3 WebCodecs `AudioDecoder` — the 2026 change

This is the meaningful new capability since the last time anyone wrote this document.

- Safari 16.4–18.7 shipped only the **video** interfaces (`VideoDecoder`, `VideoEncoder`,
  `EncodedVideoChunk`, `VideoFrame`).
- **Safari 26.0 adds `AudioEncoder` and `AudioDecoder`** — on **macOS, iOS and iPadOS**
  ([WebKit: Features in Safari 26.0](https://webkit.org/blog/17333/webkit-features-in-safari-26-0/),
  [WWDC25 beta post](https://webkit.org/blog/16993/news-from-wwdc25-web-technology-coming-this-fall-in-safari-26-beta/)).
- Android Chrome has had full WebCodecs since Chrome 94 (2021).

So as of 2026 the "decode Opus/AAC chunks yourself and push them into an AudioWorklet ring
buffer" architecture is **finally viable on iOS**. Minimum bar: iOS/iPadOS 26. That is a
big chunk of the installed base but not all of it — feature-detect and fall back.

```js
if (!('AudioDecoder' in window)) { /* fall back to WebRTC or MediaSource */ }
const dec = new AudioDecoder({
  output: (audioData) => ring.pushAudioData(audioData),   // -> AudioWorklet
  error:  (e) => console.error(e),
});
const cfg = { codec: 'opus', sampleRate: 48000, numberOfChannels: 2 };
if ((await AudioDecoder.isConfigSupported(cfg)).supported) dec.configure(cfg);
dec.decode(new EncodedAudioChunk({ type: 'key', timestamp, data }));
```

**Still: WebRTC remains the lower-effort, lower-risk path**, because WebCodecs gives you
a decoder but *no transport, no jitter buffer, no packet loss concealment, no clock drift
correction*. Writing a competent jitter buffer is the hard part of this project, and NetEq
already exists inside every browser.

### 2.4 Transport options, ranked for this project

| Transport | iOS 2026 | Notes |
|---|---|---|
| **WebRTC (`RTCPeerConnection`, Opus)** | ✅ Full | Sub-200 ms glass-to-glass achievable. Opus enc+dec under ~40 ms. Built-in jitter buffer, FEC, PLC, clock drift handling. Plugin side needs a WebRTC stack (libwebrtc, or a small SFU/`whip` sender). Signalling over a tiny local HTTP endpoint. **Recommended.** |
| **WebTransport (HTTP/3, unreliable datagrams)** | ✅ **New in Safari 26.4, March 2026** — now Baseline across Chromium/Firefox/Safari ([webrtc.ventures](https://webrtc.ventures/2026/04/webtransport-is-now-baseline-what-it-means-for-real-time-media/)) | Very attractive long-term: UDP-like datagrams with a clean API, no SDP. But **you write the jitter buffer and concealment yourself**, and it needs QUIC/TLS certs. Watch this; don't bet v1 on it. |
| **WebSocket + AudioWorklet ring buffer** | ✅ Full | Simplest to implement on the C++ side (a WebSocket server is ~200 lines). But TCP head-of-line blocking causes **100+ ms latency spikes** on lossy Wi-Fi and there's no recovery mechanism. Fine for a proof-of-concept on a clean wired-to-AP network; poor in a real studio with a busy 2.4 GHz band. |
| **HLS / LL-HLS / MSE** | ✅ | Seconds of latency. Wrong tool. |

Note also: **WebRTC Encoded Transforms / Insertable Streams are still not implemented in
Safari** ([ZEGOCLOUD](https://www.zegocloud.com/blog/apple-safari-webrtc)), so don't plan
on custom in-browser payload manipulation on iOS.

### 2.5 Autoplay / user-gesture rules — exact 2026 behaviour

- An `AudioContext` created without a user gesture starts in state `"suspended"`. You must
  call `ctx.resume()` **from inside a user-gesture event handler**
  ([MDN Autoplay guide](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Autoplay)).
- On iOS the gesture is considered complete on **`touchend`** — i.e. when the finger
  *lifts*, not on `touchstart`. Bind to `click` or `touchend`.
- One unlock is enough for the lifetime of the context. Create **one** `AudioContext` for
  the whole page and never throw it away
  ([Matt Montag's canonical writeup](https://www.mattmontag.com/web/unlock-web-audio-in-safari-for-ios-and-macos)).
- Web Audio is **muted by the hardware ringer/silent switch** on iOS;
  `<audio>`/`<video>` element playback is not
  ([WebKit bug 237322](https://bugs.webkit.org/show_bug.cgi?id=237322)).
- Regressions do happen: there are 2025 reports of `resume()` failing inside a click
  handler in installed PWAs on iOS 26.0.1
  ([Apple Developer Forums](https://developer.apple.com/forums/thread/805900)) — another
  reason to prefer a plain Safari tab over an installed PWA.

Robust unlock:

```js
let ctx;
document.getElementById('listen').addEventListener('click', async () => {
  ctx ??= new AudioContext({ latencyHint: 'interactive' });
  if ('audioSession' in navigator) navigator.audioSession.type = 'playback'; // §2.6
  if (ctx.state !== 'running') await ctx.resume();
  await silentKeepAlive.play();       // <audio loop> with 1s of silence, iOS mute-switch fix
  wakeLock = await navigator.wakeLock?.request('screen').catch(() => null);
  startTransport();
}, { once: false });

// re-acquire the wake lock when returning to the tab
document.addEventListener('visibilitychange', async () => {
  if (document.visibilityState === 'visible' && wakeLock === null)
    wakeLock = await navigator.wakeLock?.request('screen').catch(() => null);
});
```

### 2.6 The Audio Session API — use it

[`navigator.audioSession`](https://developer.mozilla.org/en-US/docs/Web/API/AudioSession)
([W3C spec](https://www.w3.org/TR/audio-session/)) lets the page declare its audio
*intent*, which is exactly what the iOS mute-switch problem needs. As of late 2025 it is
an Editor's Draft **implemented only by Safari** — which is fine, since Safari is the
platform with the problem.

Types: `auto` (default; behaves like `ambient` in Safari — this is what causes the
mute-switch muting), `playback`, `transient`, `transient-solo`, `ambient`,
`play-and-record`.

```js
if ('audioSession' in navigator) navigator.audioSession.type = 'playback';
```

Keep the looping-silent-`<audio>` fallback for older iOS
([`unmute-ios-audio`](https://github.com/feross/unmute-ios-audio),
[`unmute`](https://github.com/swevans/unmute)). Belt and braces; both are tiny.

### 2.7 Feature-detection matrix to ship in the page

Render this as a diagnostics panel — it will save you enormous support pain.

```js
const caps = {
  secureContext:      window.isSecureContext,
  crossOriginIsolated: self.crossOriginIsolated,
  audioWorklet:       'audioWorklet' in (window.AudioContext?.prototype ?? {}),
  sharedArrayBuffer:  typeof SharedArrayBuffer !== 'undefined',
  audioDecoder:       'AudioDecoder' in window,       // iOS 26+ / Chrome 94+
  webTransport:       'WebTransport' in window,       // iOS 26.4+ / Chrome 97+
  webRTC:             'RTCPeerConnection' in window,
  wakeLock:           'wakeLock' in navigator,
  audioSession:       'audioSession' in navigator,    // Safari only
};
```

---

## Part 3 — Build toolchain for the JUCE plugin

### 3.1 Version facts (verified 2026-08-27)

- Latest JUCE: **9.0.1**, published **2026-08-10**. **9.0.0** on **2026-07-21**
  ([GitHub releases API](https://github.com/juce-framework/JUCE/releases)).
- JUCE 9 highlights: lunasvg-based SVG renderer, variable fonts, a **new macOS CoreAudio
  implementation with the aggregate-device API claiming latency reductions of 11–25 ms**,
  OpenGL ES on embedded Linux, better headless/embedded builds. Pricing and licensing
  **unchanged from JUCE 8**; JUCE 8 subscriptions auto-upgraded
  ([forum announcement](https://forum.juce.com/t/juce-9-is-available-now/69175)).
- **CLAP:** the JUCE roadmap promised authoring CLAP directly
  ([Q3 2025 roadmap](https://juce.com/blog/juce-roadmap-update-q3-2025/)), but the 9.0
  announcement does not list it as shipped — sample-accurate automation and "unique CLAP
  features" are described as work-in-progress behind AudioProcessor v2. **Treat native
  CLAP as not-yet-available in 9.0.1**; if you want CLAP now, use
  [`clap-juce-extensions`](https://github.com/free-audio/clap-juce-extensions).
- Licensing: JUCE is dual-licensed **AGPLv3 / commercial**. For an open-source
  clone-and-run project, AGPLv3 is fine — **but you must then license PhonePostMix
  itself under AGPLv3** (or use the free tier's terms). State this explicitly in the
  README; it's the single most common licensing surprise for JUCE newcomers.

### 3.2 Root `CMakeLists.txt` — `FetchContent`, so nobody has to clone JUCE by hand

Cloning JUCE as a submodule or a sibling directory (as many tutorials do) is a
clonability tax. `FetchContent` makes `git clone && cmake -B build` sufficient.

```cmake
cmake_minimum_required(VERSION 3.22)

project(PhonePostMix VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)          # clangd / editor integration

# Universal binary on macOS; must be set before any target is created.
if (APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0"     CACHE STRING "" FORCE)
endif()

include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.1                # pin! never track a moving branch
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PhonePostMix
    COMPANY_NAME              "PhonePostMix"
    BUNDLE_ID                 "dev.phonepostmix.plugin"
    PLUGIN_MANUFACTURER_CODE  Ppmx        # 4 chars, >=1 uppercase
    PLUGIN_CODE               Ppm1        # 4 chars, exactly 1 uppercase
    FORMATS                   AU VST3 Standalone
    PRODUCT_NAME              "PhonePostMix"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   ${PPM_COPY_AFTER_BUILD}   # OFF in CI, ON locally
    NEEDS_WEB_BROWSER         FALSE
    NEEDS_CURL                FALSE)

target_sources(PhonePostMix PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/net/StreamServer.cpp
    source/dsp/Resampler.cpp)

target_include_directories(PhonePostMix PRIVATE source)

# Embed the receiver web page into the binary so the plugin can serve it.
juce_add_binary_data(PhonePostMixWebAssets
    HEADER_NAME WebAssets.h
    NAMESPACE   webassets
    SOURCES     web/index.html web/app.js web/worklet.js web/style.css)

target_compile_definitions(PhonePostMix PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0        # only legal under a paid or AGPL-compliant licence
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(PhonePostMix
    PRIVATE
        PhonePostMixWebAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

option(PPM_BUILD_TESTS "Build unit tests" ON)
option(PPM_COPY_AFTER_BUILD "Install plugin to the user plugin folder after build" OFF)
if (PPM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

Reference: [JUCE's own CMake AudioPlugin example](https://github.com/juce-framework/JUCE/blob/master/examples/CMake/AudioPlugin/CMakeLists.txt),
[JUCE CMake API docs](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md),
[Sudara's "How to use CMake with JUCE"](https://melatonin.dev/blog/how-to-use-cmake-with-juce/).

**CPM alternative.** [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) is a thin wrapper
over `FetchContent` that adds source caching (`CPM_SOURCE_CACHE`), which materially speeds
up repeated clean builds and CI:

```cmake
include(cmake/CPM.cmake)
CPMAddPackage(NAME JUCE GITHUB_REPOSITORY juce-framework/JUCE GIT_TAG 9.0.1)
CPMAddPackage(NAME Catch2 GITHUB_REPOSITORY catchorg/Catch2 GIT_TAG v3.7.1)
```

Either is fine. `FetchContent` is one fewer vendored file in the repo; CPM is nicer once
you have three or more dependencies. Note one report of duplicate-inclusion errors mixing
CPM and JUCE ([Spitzfaden](https://reillyspitzfaden.com/posts/2025/08/plugins-for-everyone-crossplatform-juce-with-cmake-github-actions/)).

**Key gotcha:** `COPY_PLUGIN_AFTER_BUILD TRUE` must be **off in CI** — the runner has no
plugin folder and the copy step will fail or pollute. Gate it behind an option as above.

### 3.3 Per-platform build commands

```bash
# macOS  (VST3 + AU + Standalone, universal binary)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Windows (VST3 + Standalone) — from a Developer Command Prompt, or via ilammy/msvc-dev-cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Linux  (VST3 + Standalone)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Use `--config Release` on multi-config generators (MSVC, Xcode);
`-DCMAKE_BUILD_TYPE=Release` on single-config (Ninja, Makefiles). Passing both is harmless
and makes the docs copy-pasteable across platforms.

Artefacts land at
`build/PhonePostMix_artefacts/Release/{VST3,AU,Standalone}/…`.

**AU is macOS-only** (it's a CoreAudio format). **VST3 is the only format on Windows and
Linux** unless you licence the VST2 SDK (you can't — Steinberg stopped issuing it) or add
CLAP/LV2. On Linux, JUCE also supports LV2 via `FORMATS ... LV2` if you want Ardour users
to have a native-feeling option.

### 3.4 Linux build dependencies

The perennial cause of "it doesn't build for me". Put this verbatim in the README.

```bash
sudo apt-get update && sudo apt-get install -y \
  build-essential ninja-build clang \
  libasound2-dev libjack-jackd2-dev ladspa-sdk \
  libcurl4-openssl-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev \
  libwebkit2gtk-4.1-dev libglu1-mesa-dev mesa-common-dev
```

(From the [JUCE Linux dependency thread](https://forum.juce.com/t/list-of-juce-dependencies-under-linux/15121), as used by Pamplejuce.)
If you set `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0` you can drop `libwebkit2gtk` and
`libcurl` — worth doing, since `libwebkit2gtk-4.1-dev` is a heavy and version-churny
dependency that breaks on non-Ubuntu distros.

**Use Clang on Linux** so all three platforms use a Clang-family compiler and warnings
behave consistently.

### 3.5 GitHub Actions CI matrix

A practical, complete workflow. Compare with the two canonical references:
[Pamplejuce's `build_and_test.yml`](https://github.com/sudara/pamplejuce/blob/main/.github/workflows/build_and_test.yml)
(the gold standard — codesigning, notarization, Azure Trusted Signing, installers) and
[Tracktion pluginval's own build](https://github.com/Tracktion/pluginval/blob/develop/.github/workflows/build.yaml).

```yaml
# .github/workflows/build.yml
name: build

on:
  push:
    branches: [ main ]
    tags: [ 'v*' ]
  pull_request:
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

env:
  BUILD_TYPE: Release
  BUILD_DIR: build
  DISPLAY: ":0"            # Linux pluginval needs an X display
  SCCACHE_GHA_ENABLED: "true"

defaults:
  run:
    shell: bash

jobs:
  build:
    name: ${{ matrix.name }}
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: Linux
            os: ubuntu-22.04
            pluginval-binary: ./pluginval
            extra-flags: >-
              -G Ninja
              -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
              -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld
          - name: macOS
            os: macos-14
            pluginval-binary: pluginval.app/Contents/MacOS/pluginval
            extra-flags: -G Ninja -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
          - name: Windows
            os: windows-latest
            pluginval-binary: ./pluginval.exe
            extra-flags: -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl

    steps:
      - uses: actions/checkout@v4

      - name: Set up MSVC environment (Windows)
        if: runner.os == 'Windows'
        uses: ilammy/msvc-dev-cmd@v1

      - name: Set up Clang (Linux)
        if: runner.os == 'Linux'
        uses: egor-tensin/setup-clang@v1

      - name: Install Linux deps + virtual display
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y ninja-build lld xvfb \
            libasound2-dev libjack-jackd2-dev ladspa-sdk \
            libcurl4-openssl-dev libfreetype-dev libfontconfig1-dev \
            libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
            libxinerama-dev libxrandr-dev libxrender-dev \
            libglu1-mesa-dev mesa-common-dev
          sudo /usr/bin/Xvfb $DISPLAY &

      - name: Install Ninja (macOS)
        if: runner.os == 'macOS'
        run: brew install ninja

      - name: Install Ninja (Windows)
        if: runner.os == 'Windows'
        run: choco install ninja

      - name: Cache compiler output
        uses: mozilla-actions/sccache-action@v0.0.5

      - name: Cache FetchContent (JUCE, Catch2)
        uses: actions/cache@v4
        with:
          path: ${{ env.BUILD_DIR }}/_deps
          key: deps-${{ runner.os }}-${{ hashFiles('CMakeLists.txt') }}

      - name: Configure
        run: >-
          cmake -B ${{ env.BUILD_DIR }}
          -DCMAKE_BUILD_TYPE=${{ env.BUILD_TYPE }}
          -DCMAKE_C_COMPILER_LAUNCHER=sccache
          -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
          -DPPM_BUILD_TESTS=ON
          -DPPM_COPY_AFTER_BUILD=OFF
          ${{ matrix.extra-flags }}

      - name: Build
        run: cmake --build ${{ env.BUILD_DIR }} --config ${{ env.BUILD_TYPE }} --parallel

      - name: Unit tests
        working-directory: ${{ env.BUILD_DIR }}
        run: ctest --output-on-failure --build-config ${{ env.BUILD_TYPE }}

      - name: Resolve artefact paths
        run: |
          A="${{ env.BUILD_DIR }}/PhonePostMix_artefacts/${{ env.BUILD_TYPE }}"
          echo "VST3_PATH=$A/VST3/PhonePostMix.vst3" >> $GITHUB_ENV
          echo "AU_PATH=$A/AU/PhonePostMix.component" >> $GITHUB_ENV
          echo "ARTEFACTS=$A" >> $GITHUB_ENV

      - name: pluginval (VST3)
        run: |
          curl -sLO "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_${{ runner.os }}.zip"
          7z x -y "pluginval_${{ runner.os }}.zip"
          ${{ matrix.pluginval-binary }} \
            --strictness-level 10 \
            --validate-in-process \
            --skip-gui-tests \
            --timeout-ms 60000 \
            --output-dir "pluginval-logs" \
            --verbose \
            --validate "$VST3_PATH"

      - name: auval (macOS AU)
        if: runner.os == 'macOS'
        run: |
          mkdir -p ~/Library/Audio/Plug-Ins/Components
          cp -R "$AU_PATH" ~/Library/Audio/Plug-Ins/Components/
          killall -9 AudioComponentRegistrar || true
          # type / manufacturer / subtype — must match juce_add_plugin
          auval -v aufx Ppm1 Ppmx

      - name: pluginval (macOS AU)
        if: runner.os == 'macOS'
        run: |
          ${{ matrix.pluginval-binary }} --strictness-level 10 --skip-gui-tests \
            --validate-in-process --validate "$AU_PATH"

      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: PhonePostMix-${{ runner.os }}
          path: |
            ${{ env.ARTEFACTS }}
            pluginval-logs
```

Notes on the choices above:

- **`fail-fast: false`** — you want to see all three platforms' failures, not just the
  first.
- **`sccache`** via [`mozilla-actions/sccache-action`](https://github.com/mozilla-actions/sccache-action) —
  JUCE is a big compile; without caching, CI runs are 10–20 minutes each.
- **Cache `build/_deps`** so `FetchContent` doesn't re-clone JUCE every run.
- **Suffix artefact names with `runner.os`** — otherwise the three jobs collide on upload
  ([Spitzfaden](https://reillyspitzfaden.com/posts/2025/08/plugins-for-everyone-crossplatform-juce-with-cmake-github-actions/)).
- **`Xvfb`** on Linux: pluginval opens a GUI even with `--skip-gui-tests` in some paths.
  Starting a virtual display is cheaper than debugging why it hangs.
- **Known issue:** the same author reports Linux VST3s built on GitHub Actions being ~6×
  larger than local builds and failing to load in DAWs despite compiling and validating.
  If you hit this, check that you're stripping (`-DCMAKE_BUILD_TYPE=Release`, and
  `strip --strip-unneeded` on the `.so` inside the bundle) and that LTO isn't producing a
  fat unstripped object. Treat Linux CI artefacts as unverified until someone loads one.

### 3.6 pluginval in CI

[pluginval](https://github.com/Tracktion/pluginval) is Tracktion's cross-platform plugin
validator, and the de-facto standard gate.

Options that matter
([docs](https://github.com/Tracktion/pluginval/blob/develop/docs/Testing%20plugins%20with%20pluginval.md),
[Adding pluginval to CI](https://github.com/Tracktion/pluginval/blob/develop/docs/Adding%20pluginval%20to%20CI.md)):

| Flag | Meaning |
|---|---|
| `--validate <path>` | Plugin to validate |
| `--strictness-level 1–10` | Default 5. **5 is the minimum for host compatibility.** 10 adds parameter fuzzing and repeated state restoration. |
| `--skip-gui-tests` | For headless CI |
| `--validate-in-process` | No child process — better stack traces in CI |
| `--timeout-ms N` | Default 30000; `-1` = none |
| `--output-dir <dir>` | Where log files go — **upload these as artifacts** |
| `--sample-rates 44100,48000,96000` | Comma-separated |
| `--block-sizes 64,128,512,1024` | Comma-separated |
| `--rtcheck enabled\|relaxed\|disabled` | **Real-time safety checking** — flags allocations/locks in `processBlock`. Enormously valuable for a plugin that also runs a network server. |
| `--repeat N`, `--randomise` | Shake out order-dependent bugs |

Exit code **0** = pass, **1** = any failure, so no extra scripting is needed for a CI gate.
Env vars work too (`SKIP_GUI_TESTS=1`, `TIMEOUT_MS=30000`).

For PhonePostMix specifically, `--rtcheck enabled` is the flag to care about: a naive
implementation will allocate or take a mutex inside `processBlock` when pushing samples to
the network thread. pluginval will catch that. Use a lock-free SPSC FIFO
(`juce::AbstractFifo`) between the audio thread and the network thread.

Extra reading: [Sudara, "Pluginval is a plugin dev's best friend"](https://melatonin.dev/blog/pluginval-is-a-plugin-devs-best-friend/).

### 3.7 auval in CI

`auval` (`auvaltool`) is Apple's AU conformance checker — required if you ship AU, because
Logic and GarageBand will refuse a plugin that fails it
([TN2204](https://developer.apple.com/library/archive/technotes/tn2204/_index.html)).

```bash
auval -a                      # list all AUs the system knows about
auval -v aufx Ppm1 Ppmx       # type subtype manufacturer
```

The type is `aufx` for an effect, `aumu` for an instrument, `aumf` for a MIDI-processing
effect. Subtype = your `PLUGIN_CODE`, manufacturer = your `PLUGIN_MANUFACTURER_CODE`.

**CI gotchas:**

- The AU must be in `/Library/Audio/Plug-Ins/Components/` or
  `~/Library/Audio/Plug-Ins/Components/` — `auval` will not look at your build directory.
- `AudioComponentRegistrar` caches the registry. After copying, run
  `killall -9 AudioComponentRegistrar` to force a rescan
  ([Moonbase writeup](https://moonbase.sh/articles/debugging-your-audio-unit-plugin-with-auvaltool-aka-auval/),
  [Oli Larkin's plugin dev notes](https://gist.github.com/olilarkin/8f378d212b0a59944d84f9f47061d70f)).
- `"No types found"` means the bundle is missing, damaged, wrong architecture, or not
  discoverable — not that your DSP is wrong.
- On Apple Silicon runners, `auval` validates the arm64 slice. Add `arch -x86_64 auval …`
  if you want to prove the Intel slice of a universal binary too.

### 3.8 macOS code signing and notarisation — the minimum you need to know

You need this the moment you distribute a binary, because Gatekeeper will block an
unsigned/unnotarised plugin downloaded from the internet.

**What you need:**

- Apple Developer Program membership ($99/yr).
- A **Developer ID Application** certificate (for the `.vst3` / `.component` / `.app`).
- A **Developer ID Installer** certificate (only if you ship a `.pkg`).
- An **app-specific password** or an App Store Connect API key for `notarytool`.

**The four steps:**

```bash
# 1. Sign each bundle with the hardened runtime and a secure timestamp.
codesign --force --deep --strict --options=runtime --timestamp \
  -s "Developer ID Application: Your Name (TEAMID)" \
  "build/PhonePostMix_artefacts/Release/VST3/PhonePostMix.vst3"

# 2. Package. Notarisation needs a container: .zip, .dmg or .pkg.
pkgbuild --identifier dev.phonepostmix.vst3.pkg --version 0.1.0 \
  --component "…/PhonePostMix.vst3" \
  --install-location "/Library/Audio/Plug-Ins/VST3" PhonePostMix.vst3.pkg
productbuild --distribution distribution.xml \
  --sign "Developer ID Installer: Your Name (TEAMID)" --timestamp PhonePostMix.pkg

# 3. Notarise and wait for Apple's verdict.
xcrun notarytool submit PhonePostMix.pkg \
  --apple-id "$APPLE_ID" --password "$APP_SPECIFIC_PASSWORD" \
  --team-id "$TEAM_ID" --wait

# 4. Staple the ticket so it works offline.
xcrun stapler staple PhonePostMix.pkg
```

Notes:

- `altool` is dead (unsupported since autumn 2023). Use `notarytool`, introduced WWDC 2022.
- **Notarising a `.pkg` or `.dmg` covers everything inside it** — you don't notarise each
  plugin bundle separately, but you do have to **codesign** each one separately.
- If you ship a bare `.zip`, you still notarise the zip; stapling to a zip isn't possible,
  so ship a `.pkg` or `.dmg` if you want offline-friendly distribution.
- In CI you need the certs in a temporary keychain. Use
  [`sudara/basic-macos-keychain-action`](https://github.com/sudara/basic-macos-keychain-action)
  rather than hand-rolling `security create-keychain`.
- Secrets to add to the repo: `DEV_ID_APP_CERT` (base64 `.p12`), `DEV_ID_APP_PASSWORD`,
  `DEVELOPER_ID_APPLICATION` (the identity string), `NOTARIZATION_USERNAME`,
  `NOTARIZATION_PASSWORD`, `TEAM_ID`.
- **Windows equivalent:** signing now effectively requires a hardware token or a cloud
  signing service. [Azure Trusted Signing](https://github.com/Azure/trusted-signing-action)
  (~$10/mo) is what Pamplejuce uses and is the cheapest sane route.
- Guides: [Sudara, "How to code sign and notarize macOS audio plugins in CI"](https://melatonin.dev/blog/how-to-code-sign-and-notarize-macos-audio-plugins-in-ci/);
  [KVR's long-running HOWTO thread](https://www.kvraudio.com/forum/viewtopic.php?t=531663).

**For a clone-and-run open-source repo:** signing is optional for contributors. Make CI
sign **only on tags**, gated on the secrets existing, so forks and PRs still build:

```yaml
      - name: Codesign (macOS, release only)
        if: runner.os == 'macOS' && startsWith(github.ref, 'refs/tags/v') && env.DEVELOPER_ID_APPLICATION != ''
```

And document the local escape hatch for users on unsigned dev builds:
`xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/PhonePostMix.vst3`.

---

## Part 4 — Testing an audio plugin with no commercial DAW

### 4.1 Free / open hosts

| Host | Licence / cost | Formats | Platforms | Best for |
|---|---|---|---|---|
| **[JUCE AudioPluginHost](https://github.com/juce-framework/JUCE/tree/master/extras/AudioPluginHost)** | Comes with JUCE — you already have the source | VST3, AU, LADSPA, LV2 | mac/Win/Linux | **The default dev loop.** Tiny, launches instantly, lets you attach a debugger and set breakpoints in `processBlock`. Build it from the JUCE you already fetched. |
| **[pluginval](https://github.com/Tracktion/pluginval)** | GPLv3, free | VST3, AU, VST, LV2 | mac/Win/Linux | Automated conformance + real-time-safety gate. Not a listening host. |
| **[Carla](https://kx.studio/Applications:Carla)** | GPLv2+, free | VST2/3, AU, LV2, LADSPA, DSSI, CLAP, JACK | mac/Win/Linux | Best *free* host for actually routing audio around; excellent on Linux; scriptable via OSC. |
| **[Ardour](https://ardour.org/)** | GPLv2+, free (source build free; binaries ask for a small payment) | VST2/3, AU (mac), LV2 | mac/Win/Linux | A real DAW for realistic session testing. Distro packages are free on Linux. |
| **[REAPER](https://reaper.fm/)** | **Fully functional 60-day evaluation, no restrictions or watermark**; $60 discounted / $225 commercial licence afterwards | VST2/3, AU, CLAP, LV2, JS | mac/Win/Linux | Closest thing to "what real users use". Excellent scripting (ReaScript) for automated session tests. **Note: it's an evaluation, not free software** — don't tell contributors it's free, tell them it's a 60-day trial. |
| **[Steinberg VST3PluginTestHost](https://steinbergmedia.github.io/vst3_dev_portal/pages/What+is+the+VST3+SDK/Plug-in+Test+Host.html)** | Free with the VST3 SDK | VST3 | mac/Win | The reference VST3 host + Steinberg's own validator. Good for "is my VST3 spec-compliant" disputes. |
| **JUCE Standalone target** | Free | n/a | all | Add `Standalone` to `FORMATS` and you get an app that opens the audio device directly. **For PhonePostMix this is arguably the primary artefact** — it lets people try the phone streaming with no DAW at all. |

**Recommended stack for this repo:** `AudioPluginHost` for the inner loop, the
`Standalone` target for demoing, `pluginval` in CI, and Reaper's trial for the final
"does it behave in a real DAW" check before a release.

### 4.2 Headless / automated testing of `processBlock`

You do not need a host to exercise the processor. `AudioProcessor` is just a class:

```cpp
// tests/ProcessBlockTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "PluginProcessor.h"

TEST_CASE ("processBlock is silent-in-silent-out and never emits NaN", "[dsp]")
{
    PhonePostMixProcessor proc;

    constexpr double sr = 48000.0;
    constexpr int    bs = 512;
    proc.setPlayConfigDetails (2, 2, sr, bs);
    proc.prepareToPlay (sr, bs);

    juce::AudioBuffer<float> buffer (2, bs);
    juce::MidiBuffer midi;

    buffer.clear();
    proc.processBlock (buffer, midi);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto s = buffer.getSample (ch, i);
            REQUIRE (std::isfinite (s));
            REQUIRE_THAT (s, Catch::Matchers::WithinAbs (0.0f, 1.0e-7f));
        }

    proc.releaseResources();
}

TEST_CASE ("survives block-size and sample-rate churn", "[dsp]")
{
    PhonePostMixProcessor proc;
    for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (int bs : { 1, 16, 32, 64, 128, 480, 512, 1024, 2048 })
        {
            proc.setPlayConfigDetails (2, 2, sr, bs);
            proc.prepareToPlay (sr, bs);
            juce::AudioBuffer<float> b (2, bs);
            juce::MidiBuffer m;
            b.clear();
            proc.processBlock (b, m);          // must not crash or assert
            proc.releaseResources();
        }
}

TEST_CASE ("state round-trips", "[state]")
{
    PhonePostMixProcessor a, b;
    a.getParameters()[0]->setValueNotifyingHost (0.37f);

    juce::MemoryBlock blob;
    a.getStateInformation (blob);
    b.setStateInformation (blob.getData(), (int) blob.getSize());

    REQUIRE (b.getParameters()[0]->getValue() == Catch::Approx (0.37f));
}
```

`tests/CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(Tests
    Main.cpp                 # custom main, see below
    ProcessBlockTests.cpp
    ResamplerTests.cpp
    RingBufferTests.cpp)

target_link_libraries(Tests PRIVATE
    PhonePostMix                     # the shared-code target juce_add_plugin created
    Catch2::Catch2
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

target_compile_definitions(Tests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(Tests)
```

`tests/Main.cpp` — you need a custom main so JUCE's singletons initialise/shut down
cleanly (otherwise you get leak assertions at exit):

```cpp
#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;      // RAII init/shutdown
    return Catch::Session().run (argc, argv);
}
```

Then `ctest --output-on-failure` in CI (already in the workflow above).
References: [ejaaskel, "Unit testing audio processors with JUCE & Catch2"](https://ejaaskel.dev/unit-testing-audio-processors-with-juce-catch2/),
[juce-cookbook unit tests chapter](https://tobanteaudio.gitbook.io/juce-cookbook/testing/unit_test),
[Sinecure-Audio/TestsTalk](https://github.com/Sinecure-Audio/TestsTalk).

### 4.3 What to actually assert about DSP

Generic "it didn't crash" tests are cheap but weak. The high-value assertions for a DSP
project:

1. **Finiteness / denormal hygiene** — no `NaN`, no `Inf`, and check that
   `juce::ScopedNoDenormals` is in `processBlock`. Feed it a decaying impulse and confirm
   the CPU cost doesn't explode.
2. **Silence in → silence out**, and **impulse response** matched against a golden vector
   checked into the repo (`tests/golden/*.f32`). This is the single best regression test
   for a filter or resampler.
3. **Energy / gain**: for a unity-gain path, assert RMS in ≈ RMS out within a tolerance.
4. **Latency reporting**: if you buffer, `getLatencySamples()` must be correct — assert
   that a delta at sample *n* arrives at sample *n + latency*.
5. **Sample-rate and block-size invariance** (the churn test above). Hosts call
   `prepareToPlay` with block sizes you did not anticipate, including 1.
6. **Real-time safety**: no allocation, no locks, no file/network I/O, no logging in
   `processBlock`. Enforce via `pluginval --rtcheck enabled`, and consider
   [`RealtimeSanitizer`/`rtsan`](https://clang.llvm.org/docs/RealtimeSanitizer.html)
   (in Clang 20+) with `[[clang::nonblocking]]` on `processBlock` — this is the modern,
   compiler-enforced version of the rule and it's a genuinely good fit for a plugin that
   also runs a network stack.
7. **Thread-safety of the audio↔network handoff**: run a test that hammers the SPSC FIFO
   from two threads under TSan.
8. **Benchmarks** — Catch2 has `BENCHMARK`; track `processBlock` cost per release so
   nobody silently regresses CPU by 3×.

Sanitizer job worth adding to CI (Linux only, cheap):

```yaml
  sanitizers:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      # …deps as above…
      - run: |
          cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPPM_BUILD_TESTS=ON \
            -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
          cmake --build build-asan
          cd build-asan && ctest --output-on-failure
```

---

## Part 5 — Repo and documentation conventions that make this clonable

The target: **a competent stranger, on any of the three desktop OSes, gets a working
plugin in under 10 minutes without asking you anything.** That's the standard
([freeCodeCamp README guide](https://www.freecodecamp.org/news/how-to-structure-your-readme-file/),
[standard-readme spec](https://github.com/RichardLitt/standard-readme/blob/main/spec.md)).

### 5.1 Repository layout

```
phonepostmix/
├── README.md                  # the contract with your reader
├── LICENSE                    # AGPL-3.0 if using JUCE's free tier — say so loudly
├── CMakeLists.txt             # one file, top-level, no Projucer
├── CMakePresets.json          # so `cmake --preset default` just works
├── .github/workflows/
│   ├── build.yml
│   └── release.yml
├── source/
│   ├── PluginProcessor.{h,cpp}
│   ├── PluginEditor.{h,cpp}
│   ├── net/                   # streaming server, signalling
│   └── dsp/                   # resampler, ring buffer — pure, testable, no JUCE GUI
├── web/                       # the phone receiver: index.html, app.js, worklet.js
├── tests/
│   ├── CMakeLists.txt
│   ├── Main.cpp
│   └── *.cpp
├── docs/
│   ├── research/              # you are here
│   ├── architecture.md
│   └── troubleshooting.md
└── scripts/
    ├── build.sh / build.ps1   # thin wrappers, not required, but nice
    └── install-deps-linux.sh
```

Design rule that pays off: **keep `source/dsp/` and `source/net/` free of JUCE GUI
dependencies** so the test target links fast and the DSP is portable.

### 5.2 README skeleton

```markdown
# PhonePostMix

One sentence: what it does. One sentence: who it's for.
[animated GIF or a screenshot of the plugin + a phone showing the receiver]

## Status
Alpha / what works / what doesn't. Be honest — it saves issues.

## How it works
A 5-line description + one diagram. DAW → plugin captures master bus → encodes →
local HTTP/WebRTC server → phone browser → your ears.
Link to docs/architecture.md for detail.

## Quick start (users)
1. Download the build for your OS from [Releases](…)
2. macOS: `xattr -dr com.apple.quarantine …` (dev builds are unsigned)
3. Put it in <the exact path, per OS, in a table>
4. Open your DAW, add PhonePostMix to the master bus
5. Scan the QR code with your phone
6. Tap **Listen**. Wear wired headphones — see Latency below.

## Quick start (build from source)
### Prerequisites
| OS | Needs |
|---|---|
| macOS 11+ | Xcode Command Line Tools, CMake ≥ 3.22 (`brew install cmake ninja`) |
| Windows 10+ | Visual Studio 2022 (Desktop C++ workload), CMake ≥ 3.22 |
| Linux | `./scripts/install-deps-linux.sh` (or the apt line below), CMake ≥ 3.22, Clang |

### Build
```bash
git clone https://github.com/you/phonepostmix.git
cd phonepostmix
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
JUCE 9.0.1 is fetched automatically — no submodules, no Projucer.
First build takes ~10 min; later builds are fast.
Artefacts appear in `build/PhonePostMix_artefacts/Release/`.

### Run without a DAW
`build/PhonePostMix_artefacts/Release/Standalone/PhonePostMix` — pick an input device,
scan the QR code.

## Testing
`ctest --test-dir build --output-on-failure`
`pluginval --strictness-level 10 --validate build/.../PhonePostMix.vst3`
Free hosts we test against: JUCE AudioPluginHost, Carla, Ardour, Reaper (60-day trial).

## Latency
A table of measured numbers, and the sentence "AirPods add 80–220 ms; use wired
headphones if latency matters." Set expectations before people file issues.

## Troubleshooting
Link to docs/troubleshooting.md. Cover at minimum:
plugin not showing up in the DAW (rescan / correct folder / Gatekeeper);
phone can't reach the page (firewall, AP client isolation, wrong subnet);
no sound on iPhone (silent switch, tap Listen, page must stay in foreground).

## Contributing / Architecture / License
AGPL-3.0 because JUCE. Explain what that means for anyone forking this.
```

### 5.3 Conventions worth adopting

- **`CMakePresets.json`.** Turns three OS-specific command lines into `cmake --preset
  default && cmake --build --preset default`. Enormous clonability win, zero cost.
- **Pin every dependency to a tag** (`GIT_TAG 9.0.1`, not `master`). A repo that breaks
  next month because upstream moved is worse than one that's a version behind.
- **Never require the Projucer.** `.jucer` files in a repo are a fork-hostile artefact;
  CMake is the supported path and is what CI uses.
- **Exact install paths, per OS, in a table** — this is the #1 support question for audio
  plugins:

  | OS | VST3 | AU |
  |---|---|---|
  | macOS | `~/Library/Audio/Plug-Ins/VST3/` | `~/Library/Audio/Plug-Ins/Components/` |
  | Windows | `C:\Program Files\Common Files\VST3\` | — |
  | Linux | `~/.vst3/` | — |

- **A `docs/troubleshooting.md`** covering Gatekeeper quarantine, DAW plugin rescans, and
  the Wi-Fi/AP-client-isolation problem. Most of your issue tracker will be these three.
- **CI badge in the README**, and make CI run on PRs from forks — a green badge is how a
  stranger decides your repo is alive.
- **A `LICENSE` discussion, not just a file.** JUCE's AGPLv3 is genuinely load-bearing
  here and people will ask.
- **Record the "why" in `docs/`** (as you're doing) so decisions like "why a URL and not
  an app" don't get relitigated in every issue.
- **A one-command demo**: the Standalone target means someone can evaluate the whole idea
  without installing a DAW. Lead the README with it.

---

## Consolidated recommendations for PhonePostMix

| Decision | Recommendation | Confidence |
|---|---|---|
| Receiver | Plain URL in mobile Safari/Chrome. Add PWA manifest later, cosmetic only. No native app in v1. | High |
| Transport | WebRTC (Opus). WebSocket+AudioWorklet only as a debug/PCM mode. Watch WebTransport (Baseline as of Safari 26.4, March 2026) for v2. | High |
| SharedArrayBuffer | Not needed if you use WebRTC. If you do use it: pass the SAB in `processorOptions`, never via `port.postMessage`; you need COOP/COEP + HTTPS. | High |
| WebCodecs `AudioDecoder` | Now available on iOS as of **Safari 26.0**. Viable but you'd be writing your own jitter buffer. Feature-detect, don't require. | High |
| Screen | Hold `navigator.wakeLock`; requires a secure context — plan for HTTPS from the plugin. | High |
| iOS audio session | Set `navigator.audioSession.type = 'playback'` + looping-silent-`<audio>` fallback for the mute switch. | High |
| Background audio | Not achievable on iOS from a web page. Document as a known limitation. | High |
| Latency messaging | Tell users to use wired headphones, in the README and in the app. BT dwarfs everything else. | High |
| Build | CMake ≥ 3.22 + JUCE 9.0.1 via `FetchContent`. Formats: `AU VST3 Standalone` (+ `LV2` for Linux if cheap). No Projucer, no submodules. | High |
| CI | GH Actions matrix ubuntu-22.04 / macos-14 / windows-latest, Ninja + Clang everywhere, sccache, `ctest`, `pluginval --strictness-level 10 --rtcheck enabled`, `auval` on macOS. Sign only on tags. | High |
| Tests | Catch2 v3 + `catch_discover_tests`, golden impulse responses, sample-rate/block-size churn, TSan/ASan job, `rtsan` on `processBlock`. | High |
| Hosts for manual testing | JUCE AudioPluginHost (inner loop), Standalone target (demo), Carla/Ardour (free), Reaper 60-day eval (realism). | High |

---

## Sources

**Receiver / web platform**
- [Screen Wake Lock supported in all browsers — web.dev](https://web.dev/blog/screen-wake-lock-supported-in-all-browsers)
- [caniuse: Screen Wake Lock API](https://caniuse.com/wake-lock)
- [WebKit bug 254545 — Wake Lock in Home Screen web apps](https://bugs.webkit.org/show_bug.cgi?id=254545)
- [MDN: Secure Contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Defenses/Secure_Contexts)
- [MDN: Autoplay guide for media and Web Audio APIs](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Autoplay)
- [Matt Montag — Unlock Web Audio in Safari for iOS and macOS](https://www.mattmontag.com/web/unlock-web-audio-in-safari-for-ios-and-macos)
- [WebKit bug 237322 — Web Audio muted when iOS ringer is muted](https://bugs.webkit.org/show_bug.cgi?id=237322)
- [MDN: AudioSession API](https://developer.mozilla.org/en-US/docs/Web/API/AudioSession) · [W3C Audio Session spec](https://www.w3.org/TR/audio-session/) · [nattog.dev — Avoiding unmuting iOS devices for the Web Audio API](https://nattog.dev/blog/web-audio-ios-unmute)
- [feross/unmute-ios-audio](https://github.com/feross/unmute-ios-audio) · [swevans/unmute](https://github.com/swevans/unmute)
- [caniuse: AudioWorklet](https://caniuse.com/mdn-api_audioworklet) · [caniuse: BaseAudioContext.audioWorklet](https://caniuse.com/mdn-api_baseaudiocontext_audioworklet)
- [MDN: AudioContext.baseLatency](https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/baseLatency) · [outputLatency](https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/outputLatency)
- [MDN: SharedArrayBuffer](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer)
- [WebKit bug 237144 — SAB in AudioWorkletProcessor (RESOLVED FIXED, Feb 2022)](https://bugs.webkit.org/show_bug.cgi?id=237144)
- [gzuidhof/coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker) · [tomayac — COOP/COEP on static hosting](https://blog.tomayac.com/2025/03/08/setting-coop-coep-headers-on-static-hosting-like-github-pages/)
- [WebKit — Features in Safari 26.0 (AudioEncoder/AudioDecoder)](https://webkit.org/blog/17333/webkit-features-in-safari-26-0/) · [WWDC25 Safari 26 beta post](https://webkit.org/blog/16993/news-from-wwdc25-web-technology-coming-this-fall-in-safari-26-beta/)
- [WebTransport is now Baseline (Safari 26.4, March 2026) — webrtc.ventures](https://webrtc.ventures/2026/04/webtransport-is-now-baseline-what-it-means-for-real-time-media/)
- [WebRTC vs WebSocket A/V sync — getstream.io](https://getstream.io/blog/webrtc-websocket-av-sync/) · [WebRTC latency — nanocosmos](https://www.nanocosmos.net/blog/webrtc-latency/)
- [Apple Safari WebRTC: what developers need to know — ZEGOCLOUD](https://www.zegocloud.com/blog/apple-safari-webrtc)
- [PWA iOS limitations & Safari support 2026 — MagicBell](https://www.magicbell.com/blog/pwa-ios-limitations-safari-support-complete-guide)
- [Apple Developer Forums — PWA video playback after iOS 26.0.1](https://developer.apple.com/forums/thread/805900)
- [Apple TestFlight](https://developer.apple.com/testflight/) · [TestFlight distribution guide](https://techconcepts.org/blog/testflight-guide)
- [AirPods Pro 2 latency guide](https://onlineaudiotest.com/devices/airpods-pro-2/) · [AirPods Max](https://onlineaudiotest.com/devices/airpods-max/) · [AirPods latency test](https://onlineaudiotest.com/airpods-latency-test/)
- [Android: audio latency for app developers](https://source.android.com/docs/core/audio/latency/app) · [Oboe low-latency audio](https://developer.android.com/games/sdk/oboe/low-latency-audio)
- [Best browser for Web Audio in 2026 — StageAlly](https://stageally.com/articles/best-browser-for-web-audio)
- [padenot/ringbuf.js](https://github.com/padenot/ringbuf.js)

**JUCE / toolchain**
- [JUCE releases](https://github.com/juce-framework/JUCE/releases) · [JUCE 9 announcement](https://forum.juce.com/t/juce-9-is-available-now/69175) · [JUCE roadmap Q3 2025](https://juce.com/blog/juce-roadmap-update-q3-2025/)
- [JUCE CMake API docs](https://github.com/juce-framework/JUCE/blob/master/docs/CMake%20API.md) · [JUCE CMake AudioPlugin example](https://github.com/juce-framework/JUCE/blob/master/examples/CMake/AudioPlugin/CMakeLists.txt)
- [Sudara — How to use CMake with JUCE](https://melatonin.dev/blog/how-to-use-cmake-with-juce/)
- [sudara/pamplejuce](https://github.com/sudara/pamplejuce) · [its build_and_test.yml](https://github.com/sudara/pamplejuce/blob/main/.github/workflows/build_and_test.yml)
- [Reilly Spitzfaden — Cross-platform JUCE with CMake & GitHub Actions](https://reillyspitzfaden.com/posts/2025/08/plugins-for-everyone-crossplatform-juce-with-cmake-github-actions/)
- [danielraffel/JUCE-Plugin-Starter](https://github.com/danielraffel/JUCE-Plugin-Starter)
- [free-audio/clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions)
- [cpm-cmake/CPM.cmake](https://github.com/cpm-cmake/CPM.cmake)

**Testing / validation / signing**
- [Tracktion/pluginval](https://github.com/Tracktion/pluginval) · [Testing plugins with pluginval](https://github.com/Tracktion/pluginval/blob/develop/docs/Testing%20plugins%20with%20pluginval.md) · [Adding pluginval to CI](https://github.com/Tracktion/pluginval/blob/develop/docs/Adding%20pluginval%20to%20CI.md) · [pluginval's own CI](https://github.com/Tracktion/pluginval/blob/develop/.github/workflows/build.yaml)
- [Sudara — Pluginval is a plugin dev's best friend](https://melatonin.dev/blog/pluginval-is-a-plugin-devs-best-friend/)
- [Apple TN2204 — Audio Unit validation using auval](https://developer.apple.com/library/archive/technotes/tn2204/_index.html) · [Moonbase — Debugging your AU with auvaltool](https://moonbase.sh/articles/debugging-your-audio-unit-plugin-with-auvaltool-aka-auval/) · [Oli Larkin's plugin dev notes](https://gist.github.com/olilarkin/8f378d212b0a59944d84f9f47061d70f)
- [Sudara — Code sign and notarize macOS audio plugins in CI](https://melatonin.dev/blog/how-to-code-sign-and-notarize-macos-audio-plugins-in-ci/) · [KVR macOS notarization HOWTO](https://www.kvraudio.com/forum/viewtopic.php?t=531663)
- [ejaaskel — Unit testing audio processors with JUCE & Catch2](https://ejaaskel.dev/unit-testing-audio-processors-with-juce-catch2/) · [juce-cookbook: unit tests](https://tobanteaudio.gitbook.io/juce-cookbook/testing/unit_test) · [Sinecure-Audio/TestsTalk](https://github.com/Sinecure-Audio/TestsTalk) · [sudara/melatonin_test_helpers](https://github.com/sudara/melatonin_test_helpers)
- [Carla](https://kx.studio/Applications:Carla) · [Ardour](https://ardour.org/) · [REAPER purchase/eval terms](https://www.reaper.fm/purchase.php) · [Steinberg VST3 Plug-in Test Host](https://steinbergmedia.github.io/vst3_dev_portal/pages/What+is+the+VST+3+SDK/Plug-in+Test+Host.html)

**Docs conventions**
- [standard-readme spec](https://github.com/RichardLitt/standard-readme/blob/main/spec.md) · [freeCodeCamp — How to structure your README](https://www.freecodecamp.org/news/how-to-structure-your-readme-file/)
