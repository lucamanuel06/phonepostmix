# Streaming a DAW master bus to a phone — prior art, plugin frameworks, and the engineering constraints

**Research date:** 2026-08-27
**Scope:** How Mix To Mobile / Audiomovers LISTENTO / SonoBus / SoundJack / Cleanfeed work; what to build a plugin with in 2026; the real-time-safety mechanics of doing network I/O from inside `AudioProcessor::processBlock`; OS sandboxing and permission constraints; and the validation/DAW pitfalls that bite this specific class of plugin.

---

## 0. Executive summary — what this tells you to build

If the goal is "insert a plugin on the master bus, hear it on my phone":

1. **Architecture:** sender plugin → lock-free FIFO → background encoder/network thread → UDP (LAN) or TLS/TCP-over-443 (internet relay) → receiver app on the phone with a jitter buffer + drift-correcting resampler. This is exactly the shape SonoBus, LISTENTO and Mix To Mobile all converge on. There is no other shape that works.
2. **Framework:** JUCE (AGPLv3 free tier, or Starter/Indie commercial). It is the only framework that gets you VST3 + AU + AAX + CLAP + standalone + **iOS/Android receiver app** from one codebase. JUCE 9 (July 2026) added first-party CLAP export.
3. **Big 2026 licensing change:** the **VST3 SDK is now MIT-licensed** (VST 3.8, October 2025). This removes the historical "GPLv3 or sign a Steinberg agreement" fork in the road. Combined with CLAP (MIT), AU (free with a $99/yr Apple account) and iPlug2 (zlib), a fully permissive open-source path now exists that did not exist two years ago.
4. **The cheapest fully-open-friendly path** is: **iPlug2 (zlib) or JUCE-under-AGPLv3**, targeting **VST3 (MIT) + CLAP (MIT) + AU**, transport built on **Opus (BSD) + your own UDP framing**, or vendored **AOO** (the library SonoBus uses). Total cash cost: $0, or $99/yr if you want notarized macOS builds and an iOS receiver.
5. **The nastiest constraints are not audio, they are OS policy:** macOS 15+ Local Network privacy, the AudioUnit sandbox in Logic/GarageBand, and iOS's `NSLocalNetworkUsageDescription` + multicast entitlement. Plan for these on day one — see §5.

---

## 1. Prior art: how the existing products actually work

### 1.1 Mix To Mobile (Sound on Digital)

The simplest and most directly relevant prior art — it is *literally* the product described in the brief.

| Aspect | What it does |
|---|---|
| Topology | **Sender plugin in the DAW + receiver app on iOS/Android.** No cloud, no account. |
| Transport | **Same-WiFi LAN only.** Both ends must be on the same network; the app discovers the sender automatically. |
| Codec | Advertised as **lossless** audio over WiFi. |
| Latency | "Low but appreciable" — reviewers explicitly say it is **not usable for monitoring while tracking**, i.e. we are in the hundreds-of-ms class, not the sub-30ms class. |
| Fan-out | Multiple devices can receive the same stream simultaneously; you can then chain out to e.g. a car stereo via AirPlay. |
| Controls | Mute-to-listeners button in the plugin; listener count displayed in the plugin UI; volume on the device. |

Sources:
- <https://www.production-expert.com/production-expert-1/stream-your-daw-to-your-phone-with-this-cool-software>
- <https://mixdownmag.com.au/reviews/review-mix-to-mobile/>
- <https://apps.apple.com/us/app/mix-to-mobile/id1659104489>
- <https://soundondigital.com/sonobus-vs-mix-to-mobile/> (vendor's own comparison against SonoBus)

**Design lesson:** the LAN-only, zero-account, auto-discovery model is what makes this product feel instant. The entire "server" problem disappears. The cost is that it does not work for a remote client across the internet — which is the gap LISTENTO fills.

### 1.2 Audiomovers LISTENTO

The professional/remote-collaboration end of the same idea.

| Aspect | What it does |
|---|---|
| Plugin formats | **AU, AAX, VST, VST3** — transmitter *and* a separate free receiver plugin. |
| Topology | **Server-relayed, not peer-to-peer.** "A dozen servers spread across the globe." The transmitter sends **one** unicast stream up; the server fans it out to all listeners. This is why the sender's uplink doesn't degrade with listener count. |
| Firewall strategy | Custom transmission protocol over **standard HTTPS ports** so no corporate IT changes are needed. This is a deliberate, important design decision for studio/post environments. |
| Codecs | **PCM 16/24/32-bit** (lossless), **AAC** at 96/128/192/256/320 kbps, **Opus** at 128/256 kbps. Adaptive bitrate based on network conditions. |
| Sample rates / channels | Up to 192 kHz (newer versions up to 384 kHz); 16 channels, and **up to 128 channels lossless** on the Pro tier — surround/Atmos post workflows. |
| Latency | **User-selectable buffer**, from ~50 ms up to ~2 seconds. (Marketing copy quoting "0.05 ms" is a unit slip for 50 ms of *added buffer*; treat the honest figure as 50 ms + network RTT + codec delay.) |
| Receivers | Web browser player (no install, no account for the listener), desktop app, iOS/Android app, or the receiver plugin so a remote engineer can record the stream into their own DAW. |

Sources:
- <https://audiomovers.com/listento> · User guide PDF: <https://audiomovers.com/storage/pdfs/LISTENTO%20User%20Guide.pdf>
- <https://www.production-expert.com/production-expert-1/audiomovers-listento-for-audio-post>
- <https://tapeop.com/reviews/gear/139/listento-remote-monitoring-plug-in>
- <https://www.audiomovers.com/help/faq/>
- <https://www.prosoundweb.com/audiomovers-announces-new-web-transmitter-for-browser-based-collaboration/>

**Design lesson:** the relay isn't laziness, it's the feature. Unicast-up / multicast-down at the server means a mix engineer on a domestic uplink can serve 30 label execs. And "runs over 443" is what makes it deployable inside Abbey Road / Warner / NBC.

### 1.3 SonoBus (open source — read this codebase)

The single most useful reference implementation, because the full source is available and it is a JUCE plugin doing exactly this job.

| Aspect | What it does |
|---|---|
| Source | <https://github.com/sonosaurus/sonobus> — **GPLv3** |
| Framework | **JUCE 6**, on a modified public fork (`github.com/essej/JUCE`, branch `sono6good`), managed via `git-subrepo`. CMake ≥ 3.15. |
| Formats | Standalone app + **AU and VST** plugins, macOS/Windows/Linux/iOS/Android. |
| Transport | **Peer-to-peer UDP.** A connection server (default `aoo.sonobus.net`, TCP/UDP **10998/10999**) is used **only for group rendezvous / peer discovery** — audio never touches it. The standalone app embeds its own connection server so you can self-host and port-forward. |
| Streaming engine | **AOO ("Audio over OSC")** — <https://github.com/essej/aoo>, upstream <https://github.com/Spacechild1/aoo> (mirror of `git.iem.at/aoo/aoo`). |
| Codec | **Raw PCM at 16/24/32-bit**, or **Opus at 16–256 kbps per channel**, switchable *per-peer, live*. |
| Latency | Guide's rule of thumb: **40 ms is the upper bound for playing together, 25 ms is "good enough."** Audio device buffer 64/128/256 samples; **Opus adds 2.5 ms**; the rest is jitter buffer + network. |

Sources:
- <https://github.com/sonosaurus/sonobus/blob/main/README.md>
- <https://www.sonobus.net/sonobus_userguide.pdf> · <https://sonobus.net/sonobus_userguide.html>
- <https://www.linuxuprising.com/2021/02/sonobus-is-open-source-low-latency-peer.html>

#### AOO is the interesting part

AOO is a C++ library with C and C++ APIs, explicitly designed to be **embedded in audio plugins**. It gives you, for free, everything that is annoying to write:

- OSC-derived message framing over UDP, with **settable packet size** (tune small for LAN, larger for WAN).
- Codec plumbing: **PCM (multiple bit depths)** and **Opus** (chosen for low bitrate quality, tiny added latency, and **packet loss concealment**). QOA is being evaluated as a middle ground.
- **Jitter buffer** in the sink, with dynamically settable latency.
- **Clock-drift correction via a time DLL filter + dynamic resampling** — this is the part people forget. The DAW's clock and the phone's clock are not the same clock; over minutes they diverge and you either underrun or overrun. See §4.5.

Sources: <https://aoo.iem.sh/api_documentation/aoo_v2.0-pre4/> · <https://www.soundingfuture.com/en/article/aoo-low-latency-peer-peer-audio-streaming-and-messaging> · <http://lac.linuxaudio.org/2014/papers/36.pdf>

### 1.4 SoundJack

The academic/NMP (networked music performance) lineage, by Dr. Alexander Carôt (released 2006).

- Deliberately minimal: it **grabs soundcard buffers and pushes them straight out over UDP/IP "as directly as possible"** to the remote peer. No relay, no adaptive layer between you and the wire.
- Exposes soundcard buffer settings and per-network tuning parameters to the user, on the theory that the achievable latency is a property of the hardware + network, not of the software, so the software should get out of the way.
- Evaluated with the **Fraunhofer ULD (ultra-low-delay) codec** to survive narrow-band DSL without adding latency.
- It is a peer application, **not a DAW plugin** — relevant as a latency-floor reference, not as a product template.

Sources: <https://www.ianhowellcountertenor.com/soundjack-real-time-online-music> · <http://www.carot.de/Docs/TMT08.pdf> · <https://arxiv.org/pdf/1808.09405> (Improving NMP Systems Using Application-Network Collaboration)

### 1.5 Cleanfeed (for contrast — the browser/WebRTC path)

- **Pure WebRTC in Chrome.** No plugin, no download for the guest — the entire studio is the browser tab.
- **Opus up to 320 kbps stereo**, direct **peer-to-peer** connections without a media server in the path.
- Aimed at live broadcast (BBC, Global, NBCU, Warner, Discovery); has a Primetime Emmy for engineering contribution.

Sources: <https://cleanfeed.net/> · <https://blog.cleanfeed.net/commitment-to-the-highest-quality/> · <https://www.radioworld.com/tech-and-gear/cleanfeed-net-live-remotes-on-a-budget>

**Why this matters to you:** WebRTC gets you NAT traversal, DTLS/SRTP encryption, congestion control and a zero-install receiver *for free*, at the cost of (a) forced 48 kHz Opus in the media path, (b) a hard dependency on ICE/STUN/TURN infrastructure, and (c) a big C++ dependency. If you want that in a native plugin, **`libdatachannel`** is the pragmatic choice — C++17, C bindings, Data Channels + media transport, fine-grained control over STUN/TURN/DTLS/SCTP, builds for macOS/iOS/Android/Windows/Linux, and is far smaller than Google's libwebrtc. <https://github.com/paullouisageneau/libdatachannel> · <https://libdatachannel.org/> · comparison: <https://tensorworks.com.au/blog/a-brief-comparison-of-libdatachannel-and-libwebrtc/>

### 1.6 Comparison table

| | Mix To Mobile | LISTENTO | SonoBus | SoundJack | Cleanfeed |
|---|---|---|---|---|---|
| Plugin? | Yes (sender) | Yes (AU/AAX/VST/VST3, TX+RX) | Yes (AU/VST) + standalone | No (standalone) | No (browser) |
| Topology | LAN direct | **Server relay** (unicast up, fan-out down) | **P2P UDP** + rendezvous server | P2P UDP | P2P WebRTC |
| Codec | Lossless | PCM 16/24/32, AAC 96–320k, Opus 128/256k | PCM 16/24/32, Opus 16–256k/ch | Raw / Fraunhofer ULD | Opus ≤320k stereo |
| Ports/firewall | LAN | **HTTPS ports (443)** | UDP + TCP 10998/10999 | Raw UDP | ICE/STUN/TURN |
| Latency class | Hundreds of ms | 50 ms – 2 s, user-selectable | 25–40 ms target | Hardware/network floor | Broadcast-grade low |
| Receiver | iOS/Android app | Browser, app, RX plugin | App/plugin | App | Browser |
| Licence | Commercial | Commercial | **GPLv3, source available** | Free/academic | Commercial |

---

## 2. Building VST3 / AU / AAX plugins in 2026

### 2.1 The headline change: VST3 is MIT now

In **October 2025, Steinberg relicensed the VST 3.8 SDK from dual GPLv3+/proprietary to the MIT License.** ASIO simultaneously gained a GPLv3 option alongside its proprietary licence.

What this means concretely:
- No Steinberg licence agreement to sign, no logo-compliance paperwork, no "open-source or pay" fork.
- You may vendor VST3 SDK code into a closed-source product **or** into a permissively-licensed open-source product.
- "Unlike VST2, VST 3 can't be discontinued; its source will always be available under an open licence."

Sources: <https://cdm.link/open-steinberg-vst3-and-asio/> · <https://www.kvraudio.com/news/steinberg-moves-vst-3-sdk-to-mit-open-source-license-asio-now-gplv3-65179> · <https://www.soundonsound.com/news/steinberg-adopt-mit-license-vst3> · <https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/> · <https://steinbergmedia.github.io/vst3_dev_portal/pages/VST+3+Licensing/VST3+License.html>

### 2.2 Format-by-format licence and cost reality

| Format | Licence | Cost | Notes |
|---|---|---|---|
| **VST3** | **MIT** (since VST 3.8 / Oct 2025) | $0 | Universally hosted. No agreement needed. |
| **CLAP** | **MIT**, community-owned (Bitwig + u-he, launched 2022) | $0 | Pure C headers, language-agnostic. Hosted by Bitwig, REAPER 7+, FL Studio 2024+, Studio One (partial), MultitrackStudio. Ableton Live: no announcement. |
| **AU / AUv3** | Free from Apple | **$99/yr Apple Developer** (needed anyway for signing/notarization) | Required for Logic/GarageBand. AUv3 is the iOS story and is sandboxed. |
| **AAX** | SDK usable under **GPLv3** for open source, or commercial | Avid Developer Program registration (paid application); **PACE signing** — normally $500/yr, waived/enrolled free when you're in Avid's program | **Unsigned AAX will not load in Pro Tools, full stop.** This is the one format you cannot ship without a gatekeeper's cooperation. |
| **LV2** | ISC/GPL | $0 | Linux-relevant only. |

Sources: <https://cleveraudio.org/developers-getting-started/> · <https://github.com/free-audio/clap> · <https://en.wikipedia.org/wiki/CLever_Audio_Plug-in> · <https://producergrid.com/blog/clap-plugin-format-everything-you-need-to-know/> · <https://www.kvraudio.com/forum/viewtopic.php?t=479249> · <https://forum.hise.audio/topic/8190/how-i-got-my-plugin-codesigned-for-aax> · <https://note.com/kawato3/n/ne11473420ad5?hl=en>

### 2.3 Frameworks

#### JUCE (now at **JUCE 9**, released 21 July 2026)

JUCE 9 shipped with, among other things:
- **First-party CLAP authoring** — CLAP is now a build target alongside VST3/AU/AAX/Standalone, so `clap-juce-extensions` is no longer strictly required.
- Sample-accurate automation read *and write* from `processBlock`.
- New SVG parser, variable fonts, **new macOS CoreAudio implementation**, faster software renderer, OpenGL ES on Linux, better multi-touch.

Sources: <https://forum.juce.com/t/juce-9-is-available-now/69175> · <https://www.kvraudio.com/news/juce-9-now-available-67802> · <https://juce.com/blog/juce-roadmap-update-q3-2025/>

**Licensing (this is the part people get wrong).** JUCE modules are **dual-licensed: AGPLv3 OR a commercial JUCE licence.** The bundled examples are ISC.

Current commercial tiers (per <https://juce.com/get-juce/> and <https://juce.com/legal/juce-8-licence/>):

| Tier | Price | Annual revenue/funding limit |
|---|---|---|
| Starter | Free | up to **$20,000** |
| Indie | **$40/mo** (1-month min) or **$800 perpetual** | up to **$300,000** |
| Pro | **$175/mo** or **$3,500 perpetual** | No limit |
| Educational | Free | up to $20,000, qualifying institutions/students |

Existing JUCE 4–8 licence holders get 30% off JUCE 9 perpetual upgrades.

Two important gotchas in the EULA:
1. The revenue limit is **entity-wide gross revenue including affiliates**, "whether or not received in connection with the entity's use of the Framework." Your day job's company revenue can disqualify you.
2. The commercial licence **forbids** placing the Framework under any open-source licence requiring source disclosure. So you pick a lane: **AGPLv3 (free, must open-source your plugin under AGPL) or commercial (closed, pay by tier).** JUCE 8 removed the old mandatory splash screen on the free tier.
3. Any developer *contributing to or modifying* JUCE source needs a licence seat.

Sources: <https://github.com/juce-framework/JUCE/blob/master/LICENSE.md> · <https://forum.juce.com/t/juce8-license-and-open-source-projects/60987> · <https://forum.juce.com/t/revenue-limits-for-juce-tiers/61058>

⚠️ **AGPL is a real problem for this specific product.** AGPLv3's §13 network clause means that if your *server* (relay/rendezvous) links AGPL JUCE code, you must offer its source to users interacting with it over the network. SonoBus sidesteps this by being GPLv3 all the way down and shipping its connection server as open source. If you plan a closed relay service, either buy a JUCE licence or keep the server 100% free of JUCE code.

#### iPlug2

- **zlib-like (permissive) licence** — free in closed-source projects, no revenue tiers, no corporate gatekeeper. <https://github.com/iplug2/iplug2>
- Targets **CLAP, VST2, VST3, AUv2, AUv3, AAX (Native), and WAM (Web Audio Module via emscripten/WASM)**.
- Recommended 2026 starting point is the out-of-source template repo **iPlug2OOS**, not the main repo.
- Vector-graphics IGraphics backends with GPU acceleration and HiDPI; "a better approach to concurrency" than iPlug1.
- Trade-off: **much smaller ecosystem than JUCE**, thinner networking/threading utility layer, and no mature mobile *app* story — you'd write the phone receiver separately (Swift/Kotlin, or Flutter/RN).

Sources: <https://iplug2.github.io/> · <https://iplug2-iplug2.mintlify.app/introduction> · <https://webaudioconf.github.io/papers/iplug2-desktop-plug-in-framework-meets-web-audio-modules.pdf>

#### Raw VST3 SDK

Now viable as a permissive path (MIT). But you write your own GUI, your own AU/AAX wrappers, your own everything. Only worth it if you're building one format and hate dependencies. For a product that must ship an iOS receiver, this is a false economy.

#### CLAP directly

Pure C headers, MIT, trivially embeddable. Excellent as a *second* format via a wrapper. Not viable as your *only* format — Logic, Pro Tools and Ableton don't host it.

#### DPF (DISTRHO Plugin Framework)

ISC-licensed, minimalist, LV2-first. Mentioned for completeness; wrong tool here.

### 2.4 The cheapest fully-open-source-friendly path

**Recommended:**

```
iPlug2 (zlib)  ──►  VST3 (MIT) + CLAP (MIT) + AUv2/AUv3
     +  Opus (BSD-3)  +  your own UDP framing   ← or vendor AOO
     +  libdatachannel (MPL-2.0) only if you need NAT traversal
Phone receiver: native Swift / Kotlin, or a plain browser page over WebRTC
Cash cost: $0, or $99/yr Apple Developer for notarization + App Store
```

**Pragmatic alternative** (more work saved, one licensing decision to make):

```
JUCE 9 under AGPLv3 (free, plugin must be AGPL)  ──►  VST3 + AU + CLAP + Standalone + iOS/Android receiver from ONE codebase
   ... or JUCE Starter (free, ≤$20k revenue) / Indie ($800 perpetual) to stay closed-source
```

The JUCE path's decisive advantage for *this* product: **the phone receiver is the same codebase.** JUCE builds iOS and Android apps with working audio I/O. With iPlug2 you write the receiver twice.

**Skip AAX in v1.** The Avid program + PACE signing is the only genuinely gated, genuinely expensive step, and Pro Tools users are not the phone-listening demographic. Add it once there's revenue.

---

## 3. Mechanics of a JUCE audio plugin doing network I/O

### 3.1 The one rule

> **You should never lock ANYTHING in your process callback, not even a mutex.**
> — JUCE forum, the canonical statement of the rule (<https://forum.juce.com/t/reading-writing-values-lock-free-to-from-processblock/50947>)

Extended, the audio thread must not: allocate, free, lock, do file I/O, do socket I/O, log, or call anything that might do those. `socket()`, `sendto()`, `connect()` and every Opus encoder-*creation* call are all forbidden in `processBlock`. Sockets and encoders get created in `prepareToPlay` or on the background thread; `processBlock` only ever touches a preallocated lock-free FIFO.

Note also: `prepareToPlay` is **not** called from the audio thread, so anything written there and read in `processBlock` is a data race that ThreadSanitizer will flag unless you make it atomic or guarantee the host's ordering. See <https://forum.juce.com/t/preparetoplay-and-processblock-thread-safety/32193>.

### 3.2 `juce::AbstractFifo` — the correct primitive

`AbstractFifo` is a **single-reader, single-writer** lock-free index manager. It holds *no data itself*; it manages positions into a buffer you own. That's exactly what you want: one audio-thread writer, one network-thread reader.

<https://docs.juce.com/master/classAbstractFifo.html>

```cpp
// AudioRingBuffer.h  — interleaved float ring shared audio-thread → network-thread
class AudioRingBuffer
{
public:
    void setSize (int numChannels, int numFrames)
    {
        channels = numChannels;
        buffer.setSize (numChannels, numFrames, false, true, true); // clears, avoids realloc later
        fifo.setTotalSize (numFrames);
    }

    // ---- called ONLY from processBlock (audio thread) ----
    // Returns frames dropped, so the UI can show an overflow indicator.
    int write (const juce::AudioBuffer<float>& src) noexcept
    {
        const int n = src.getNumSamples();
        int start1, size1, start2, size2;
        fifo.prepareToWrite (n, start1, size1, start2, size2);

        for (int ch = 0; ch < channels; ++ch)
        {
            const float* in = src.getReadPointer (juce::jmin (ch, src.getNumChannels() - 1));
            if (size1 > 0) buffer.copyFrom (ch, start1, in,         size1);
            if (size2 > 0) buffer.copyFrom (ch, start2, in + size1, size2);
        }

        fifo.finishedWrite (size1 + size2);
        return n - (size1 + size2);           // > 0 means the network thread fell behind
    }

    // ---- called ONLY from the network/encoder thread ----
    int read (juce::AudioBuffer<float>& dst, int wanted) noexcept
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead (wanted, start1, size1, start2, size2);

        for (int ch = 0; ch < channels; ++ch)
        {
            if (size1 > 0) dst.copyFrom (ch, 0,     buffer, ch, start1, size1);
            if (size2 > 0) dst.copyFrom (ch, size1, buffer, ch, start2, size2);
        }

        fifo.finishedRead (size1 + size2);
        return size1 + size2;
    }

    int getNumReady() const noexcept { return fifo.getNumReady(); }

private:
    juce::AbstractFifo fifo { 1 };
    juce::AudioBuffer<float> buffer;
    int channels = 2;
};
```

Historical note on the class's origin and intent (it was introduced alongside `AudioFormatWriter::ThreadedWriter` for exactly this "stream audio off the audio thread" job): <https://git.kx.studio/DISTRHO/JUCE/commit/0e2e4e7c3a2f3f1f552f941d85700c80fe273876>

**Common bug:** calling `prepareToWrite`/`prepareToRead` from more than one thread each. `AbstractFifo` is SRSW only — two producers corrupts it silently and you get `EXC_BAD_ACCESS` in the wild (<https://forum.juce.com/t/exc-bad-access-when-pushing-to-abstractfifo/42046>). If you need multi-producer, use one FIFO per producer.

### 3.3 `processBlock` — what it may and may not do

```cpp
void MyStreamProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer&) override
{
    juce::ScopedNoDenormals noDenormals;

    // Never allocate here. Never lock. Never touch a socket.
    if (streamingEnabled.load (std::memory_order_relaxed))
    {
        const int dropped = ring.write (buffer);
        if (dropped > 0)
            overflowCount.fetch_add (1, std::memory_order_relaxed);  // UI reads this

        // Wake the network thread WITHOUT blocking. WaitableEvent::signal() is
        // not formally documented as lock-free on every platform; the safest
        // pattern is a spin/poll on the network thread with a short timed wait,
        // or a semaphore known to be futex-backed on your targets.
        networkThreadEvent.signal();
    }

    // This is an insert on the master bus: audio must pass through untouched.
    // Do NOT clear the buffer, do NOT alter gain. The plugin is a tap.
}
```

The `WaitableEvent::signal()`-from-`processBlock` question is genuinely debated — see <https://forum.juce.com/t/audioprocessor-processblock-and-thread-notify-is-it-lock-free/27048>. The conservative approach for a shipping product: have the network thread `wait(1)` in a loop and poll `ring.getNumReady()`. A 1 ms poll costs nothing and removes an entire class of priority-inversion risk.

Also declare yourself a pass-through so hosts and validators behave:

```cpp
bool acceptsMidi() const override                 { return false; }
bool producesMidi() const override                { return false; }
double getTailLengthSeconds() const override      { return 0.0; }

// Master bus = stereo in / stereo out, matched layouts. Reject everything else
// clearly rather than half-supporting it.
bool isBusesLayoutSupported (const BusesLayout& layouts) const override
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;   // in must equal out
}
```

### 3.4 The network / encoder thread

```cpp
class StreamSenderThread : public juce::Thread
{
public:
    StreamSenderThread (AudioRingBuffer& r) : juce::Thread ("audio-net-tx"), ring (r) {}

    void run() override
    {
        // Allocation, socket creation and encoder creation happen HERE, never in processBlock.
        juce::DatagramSocket socket;
        socket.bindToPort (0);

        int err = 0;
        OpusEncoder* enc = opus_encoder_create (48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        opus_encoder_ctl (enc, OPUS_SET_BITRATE (192000));
        opus_encoder_ctl (enc, OPUS_SET_SIGNAL (OPUS_SIGNAL_MUSIC));
        // 5 ms frames @48k = 240 frames → 5 + 2.5 = 7.5 ms algorithmic delay
        const int frameSize = 240;

        juce::AudioBuffer<float> scratch (2, frameSize);
        std::vector<float>       interleaved ((size_t) frameSize * 2);
        std::vector<uint8_t>     packet (1500);

        while (! threadShouldExit())
        {
            if (ring.getNumReady() < frameSize) { wait (1); continue; }

            ring.read (scratch, frameSize);
            interleave (scratch, interleaved);

            const int bytes = opus_encode_float (enc, interleaved.data(), frameSize,
                                                 packet.data() + kHeaderBytes,
                                                 (opus_int32) packet.size() - kHeaderBytes);
            if (bytes > 0)
            {
                writeHeader (packet.data(), seq++, sampleClock, numChannels, sampleRate);
                socket.write (destHost, destPort, packet.data(), kHeaderBytes + bytes);
                sampleClock += frameSize;
            }
        }

        opus_encoder_destroy (enc);
    }
private:
    AudioRingBuffer& ring;
    uint32_t seq = 0; uint64_t sampleClock = 0;
};
```

JUCE's socket classes: <https://docs.juce.com/master/classDatagramSocket.html> (UDP) and <https://docs.juce.com/master/classStreamingSocket.html> (TCP). Both are fine — **on a background thread only.**

**Thread priority.** Do *not* run this at real-time priority; you'll fight the audio thread for cores. `Thread::startThread (Thread::Priority::high)` is right — above normal so packets go out on time, below the audio callback so you never preempt it.

**Packet sizing.** Keep the whole datagram under the path MTU (~1500 bytes Ethernet, ~1280 safe for IPv6/tunnels) so you never fragment. At 192 kbps Opus with 5 ms frames, a frame is ~120 bytes — comfortable. Uncompressed 24-bit stereo at 48 kHz is 288 kB/s; a 5 ms frame is 1440 bytes and already at the edge, so for PCM either drop to 2.5 ms frames or accept fragmentation. AOO's "settable packet size" parameter exists precisely for this trade-off.

### 3.5 Choosing the Opus configuration

Opus's algorithmic delay is **frame size + 2.5 ms** (the CELT MDCT window overlap):

| Frame size | Algorithmic delay |
|---|---|
| 2.5 ms | **5.0 ms** |
| 5 ms | 7.5 ms |
| 10 ms | 12.5 ms |
| 20 ms (default) | 22.5 ms |

Opus scales to 5 ms delay — lower than AAC-ELD's 15 ms — versus >100 ms for MP3/Vorbis/AAC-LC/HE-AAC. Use `OPUS_APPLICATION_RESTRICTED_LOWDELAY` (disables the SILK layer and the extra lookahead) plus `OPUS_SIGNAL_MUSIC`.

Sources: <https://wiki.hydrogenaudio.org/index.php?title=Opus> · <https://arxiv.org/pdf/1602.04845> (Valin et al., *High-Quality, Low-Delay Music Coding in the Opus Codec*) · <https://arxiv.org/pdf/1602.05311>

For a **mix-checking-on-your-phone** product (not live tracking), 10–20 ms frames at 256 kbps are the better trade: fewer packets, better loss resilience, delay that nobody notices when they're listening to playback rather than playing along.

### 3.6 Sample-rate conversion and clock drift

Two separate problems:

**(a) Fixed rate conversion** — the DAW is at 44.1/88.2/96/192 kHz but Opus only encodes at 8/12/16/24/**48** kHz. Resample on the *network thread*, never in `processBlock`.

```cpp
// One interpolator PER CHANNEL — they are stateful.
std::array<juce::LagrangeInterpolator, 2> resamplers;

// ratio = inputRate / outputRate; 44100 -> 48000 gives 0.91875
const double ratio = hostSampleRate / 48000.0;
for (int ch = 0; ch < 2; ++ch)
    resamplers[ch].process (ratio, inPtr[ch], outPtr[ch], numOut);
```

`LagrangeInterpolator` is 4-point Lagrange and **stateful** — call `reset()` on any discontinuity, and give each channel its own object. <https://docs.juce.com/master/classGenericInterpolator.html> · <https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_basics/utilities/juce_Interpolators.h> · <https://forum.juce.com/t/lagrangeinterpolator-for-realtime-resampling/20054>

For quality-sensitive fixed-ratio conversion, prefer libsamplerate (BSD-2) or the resampler bundled with Opus (`opus_custom` / speex_resampler) over 4-point Lagrange. `juce::ResamplingAudioSource` exists but is an `AudioSource`, awkward to bolt onto a plugin's block flow.

**(b) Clock drift — the one that ruins the product.** Your Mac's audio clock and the phone's audio clock are independent crystals differing by tens of ppm. At 50 ppm, that's ~4.3 seconds of accumulated error per day, or one 48 kHz sample every ~0.4 s. Left alone, the receiver's jitter buffer monotonically drains or overflows and you get a click every few minutes.

The fix is what AOO does: **a time DLL filter that estimates the true rate ratio, driving dynamic (fractional, continuously-adjusted) resampling at the receiver.** Estimate `receivedSampleRate / localSampleRate` from packet timestamps smoothed with a DLL (Fons Adriaensen's delay-locked-loop technique, as used in JACK), then feed that ratio, updated slowly, into the receiver's resampler. Cheaper alternative for a mix-check product: let the buffer drift and drop/duplicate a frame when it crosses a threshold — audible occasionally, but trivially simple.

Reference: AOO docs, "Clock timing differences are adjusted via a time DLL filter + dynamic resampling" — <https://aoo.iem.sh/api_documentation/aoo_v2.0-pre4/>

### 3.7 Buffer sizes

- **Host block size is not constant.** `prepareToPlay(sampleRate, maximumExpectedSamplesPerBlock)` gives you a *maximum*, not a guarantee. Hosts routinely call with fewer, and some call with *more* under load. Size the ring for the max and never assume the block length.
- **Decouple host blocks from network frames.** The ring FIFO is exactly this decoupling; the network thread pulls fixed 240/480/960-frame chunks regardless of whether the host feeds 64 or 1024.
- **Ring size:** at minimum `maxBlockSize + networkFrameSize`, realistically **200–500 ms** of audio so a hiccup on the network thread (a GC pause on the OS, a WiFi retransmit storm) doesn't cause an overflow. At 48 kHz stereo float, 500 ms = ~192 kB. Cheap.
- **Receiver jitter buffer** is where the user-visible latency dial lives — LISTENTO exposes 50 ms → 2 s, SonoBus targets 25–40 ms on wired LAN. WiFi needs a bigger safety buffer than ethernet because of jitter and retransmits; SonoBus's guide says so explicitly.

### 3.8 Handling the stereo master bus

- The plugin is a **tap, not a processor**: copy to the ring, pass audio through unmodified. Report `getTailLengthSeconds() == 0`.
- **Do not report latency.** `setLatencySamples(0)`. The stream's latency is on the *phone*, not in the DAW's signal path. Reporting nonzero latency would make the host delay-compensate everything else for no reason.
- Handle mono gracefully (some hosts probe mono/mono during validation) — see the `isBusesLayoutSupported` above.
- **Don't ship a multi-bus/surround version in v1.** LISTENTO's 128-channel Pro tier exists for Atmos post; that's a separate product with a separate buffer strategy.

### 3.9 Offline / bounce mode

When the user bounces, `processBlock` is called **as fast as the CPU can go**. If you stream that, you'll flood the network with hours of audio in seconds and the ring will overflow instantly.

```cpp
void prepareToPlay (double sr, int maxBlock) override
{
    hostSampleRate = sr;
    ring.setSize (2, (int) (sr * 0.5) + maxBlock);

    // isNonRealtime() is only reliable AFTER prepareToPlay has been called,
    // and can change on every prepareToPlay call.
    offlineMode.store (isNonRealtime());
    if (offlineMode.load())
        sender.stopStreaming();
    else
        sender.startStreaming (hostSampleRate);
}
```

Caveats, all documented pain:
- `isNonRealtime()` "may be unreliable until `prepareToPlay()` has been called, and could change each time `prepareToPlay()` is called." <https://docs.juce.com/master/classAudioProcessor.html>
- **Studio One has been reported to leave `isNonRealtime()` false during offline render.** <https://forum.juce.com/t/studio-one-isnonrealtime-always-false-in-preparetoplay/24730> · <https://forum.juce.com/t/trouble-with-isnonrealtime-for-offline-render/18564>
- **Ableton can bounce at a different sample rate than the project's playback rate**, which has broken VST3 rendering with "strange dropouts." <https://forum.juce.com/t/ableton-offline-rendering-at-altered-sample-rate-issue/64024>

**Belt-and-braces defence:** don't trust `isNonRealtime()` alone. Also rate-limit the sender by wall-clock. If more than `1.5 × realtime` worth of samples arrives in a given wall-second, you are in an offline render regardless of what the host claims — stop streaming and show "offline render — stream paused" in the UI.

---

## 4. Is opening sockets in a plugin acceptable?

**Yes — it is established, shipping practice.** SonoBus, LISTENTO, Mix To Mobile, Source-Connect, VST Connect and Steinberg's own VST Cloud all do it. There is no plugin-format prohibition. The constraints are entirely from the *host process's* sandbox and the *OS's* privacy policy, and they are real.

### 4.1 The macOS AudioUnit sandbox (Logic, GarageBand, MainStage)

Per Apple's **Technical Note TN2312, Audio Unit Host Sandboxing Guide** (<https://developer.apple.com/library/archive/technotes/tn2312/_index.html>):

> A plugin loaded into a sandboxed host **inherits the host's sandbox restrictions.** A "Sandbox Safe Audio Component" is one that functions correctly under the most restrictive settings — no filesystem access beyond explicitly granted locations, **no network**, no kernel drivers.

Mechanics:
- Sandbox-safe AUs set `kAudioComponentFlag_SandboxSafe` in `componentFlags` of their `AudioComponentDescription`. Hosts check this flag before loading.
- A host that wants to load *non*-sandbox-safe AUs must carry `com.apple.security.temporary-exception.audio-unit-host` — which Apple warns "can completely disable the host's sandbox," and triggers a user-facing dialog.
- In practice Logic runs AUs out-of-process in **`AUHostingService` / `AUHostingServiceXPC`**, a separate UNIX process talking to Logic over XPC. <https://forums.macrumors.com/threads/native-m1-plugins-are-not-handled-natively-in-logic-pro-auhostingservice.2326554/>

**Practical implication:** **do not set `kAudioComponentFlag_SandboxSafe`** on a networking AU. You are declaring a falsehood, and hosts that trust the flag will load you into a sandbox where your sockets fail. JUCE does not set it by default; verify that in your generated `Info.plist`. Then test in Logic and GarageBand early — this is where your product either works or doesn't, and finding out late is expensive.

Related evidence of how tight the AU sandbox is: sandboxed AUv3 plugins can't reach user-selected sample files, producing "operation not permitted" on preset recall. <https://developer.apple.com/forums/thread/679409>

### 4.2 Hardened runtime, entitlements, notarization

Distinct mechanisms that are frequently confused (good explainer: <https://lapcatsoftware.com/articles/hardened-runtime-sandboxing.html>):

- **App Sandbox** — opt-in confinement. `com.apple.security.network.client` / `network.server` are *App Sandbox* entitlements.
- **Hardened Runtime** — required for notarization; blocks code injection, DYLD env vars, unsigned memory. Enabled with `--options=runtime` at signing.
- **Notarization** — Apple's malware scan. Required for anything a user downloads.

If both App Sandbox and Hardened Runtime are in play you may need entitlements for the same capability twice. <https://developer.apple.com/forums/thread/698926>

Signing recipe for plugin bundles:

```bash
codesign --force --deep --options=runtime --timestamp \
  --entitlements entitlements.plist \
  -s "Developer ID Application: Your Name (TEAMID)" \
  "MyPlugin.vst3"

# Notarize the installer/archive, not each bundle:
xcrun notarytool submit MyPlugin.pkg --keychain-profile "AC_PASSWORD" --wait
xcrun stapler staple MyPlugin.pkg
```

`entitlements.plist`:
```xml
<key>com.apple.security.network.client</key><true/>
<key>com.apple.security.network.server</key><true/>   <!-- if you listen/receive -->
```

Notarizing the enclosing PKG or DMG notarizes everything inside; a plain ZIP still needs notarizing.

Sources: <https://melatonin.dev/blog/how-to-code-sign-and-notarize-macos-audio-plugins-in-ci/> · <https://www.kvraudio.com/forum/viewtopic.php?t=531663> · <https://moonbase.sh/articles/code-signing-audio-plugins-in-2025-a-round-up/> · <https://forum.juce.com/t/prepare-plugins-for-distribution-on-macos-notarization-code-signing-etc/53124>

### 4.3 ⚠️ macOS 15 Sequoia+ Local Network privacy — the biggest new hazard

**This is the single most likely reason your LAN-mode product will "just not work" for users, with no error message.**

Since macOS 15, any app that connects to an IP on the local network, uses Bonjour/mDNS, or sends unicast/multicast on the LAN triggers a Local Network permission prompt and is blocked until granted.

The vicious details:
- **It does not use TCC.** It installs a packet filter via Network Extension. Consequently `tccutil reset` **cannot** reset it for testing — a genuine developer-workflow problem. <https://eclecticlight.co/2025/03/10/manage-privacy-protection-for-network-devices-and-others/> · <https://eclecticlight.co/2026/01/18/last-week-on-my-mac-local-network-privacy-revealed/>
- **The permission attaches to the host process, not your plugin.** The prompt says *"Ableton Live would like to find devices on your local network"* — the user has no idea it's your plugin asking, and if they've already denied it for that DAW, you're silently dead.
- `NSLocalNetworkUsageDescription` in Info.plist supplies the prompt text — but there's a documented macOS 15.1 bug where a previously-installed app's description isn't shown. <https://mjtsai.com/blog/2024/10/02/local-network-privacy-on-sequoia/>
- Reported cases of permission being ignored after reboot despite being granted. <https://developer.apple.com/forums/thread/792453>
- The audio industry already has support docs for this: Native Instruments publishes "How to grant audio apps access to your local network on macOS 15 Sequoia." <https://support.native-instruments.com/hc/en-us/articles/27553634808861>

**What to do:**
1. Detect the failure — a LAN socket that never delivers is your signal. Surface an explicit in-plugin message: *"macOS is blocking local network access for [host app name]. Open System Settings → Privacy & Security → Local Network and enable [host app name]."*
2. Write the support article before you ship. NI had to; you will too.
3. Offer a relay/internet fallback that goes out over 443 — LISTENTO's port-443 design dodges this entire class of problem, and that is not a coincidence.

### 4.4 iOS receiver app

- Discovering or connecting to LAN devices requires **`NSLocalNetworkUsageDescription`** plus **`NSBonjourServices`** listing your service types (e.g. `_mixstream._udp`) since iOS 14.
- **Custom multicast/broadcast** (or enumerating all Bonjour types) needs the **`com.apple.developer.networking.multicast`** entitlement, which must be **requested from Apple via the developer portal** — a manual approval with a turnaround. Apply early.
- Behaviour has been inconsistent across iOS 17.x/18.x; test on real hardware and multiple OS versions. <https://developer.apple.com/forums/thread/790307>
- Bonjour failures show as `NSNetServicesErrorDomain: 10 / -72008` when permission is denied — and `dnssd`-API browsing doesn't even fail cleanly on denial. <https://developer.apple.com/forums/thread/741503> · <https://developer.apple.com/forums/thread/664264>
- JUCE-specific discussion: <https://forum.juce.com/t/bonjour-on-ios-nslocalnetworkusagedescription/56413>
- Also needed: the **background audio** capability, so the stream keeps playing when the screen locks.

**Strong recommendation:** avoid multicast entirely. Use Bonjour/`NSNetService` (declared types, no special entitlement) for discovery, or skip discovery altogether and have the plugin display a short pairing code / IP that the user types into the app. Both avoid the entitlement application.

### 4.5 Windows / Android

- **Windows:** first bind triggers the Windows Defender Firewall prompt, attributed to the DAW executable. Your installer can pre-add a rule, but the DAW is the process, not you — document it.
- **Android:** `INTERNET` permission is normal; `CHANGE_WIFI_MULTICAST_STATE` if you use multicast; Android 13+ needs `NEARBY_WIFI_DEVICES` for some local-network discovery.

---

## 5. Practical pitfalls

### 5.1 Hosts that suspend the plugin

**Logic does not call `processBlock` unless there is audio in the sequencer.** If the region is silent or the transport is stopped, your callbacks stop, the ring drains, and the phone hears silence — or worse, a stalled stream that looks like a disconnect.

Sources: <https://developer.apple.com/forums/thread/674704> · <https://forum.juce.com/t/logic-pro-process-block-calls-for-synthesizer/53696> · <https://forum.juce.com/t/preview-audio-in-logic-or-pro-tools-when-transport-stopped/56454>

Related:
- Most hosts **stop calling `processBlock` entirely when host-bypass is engaged**, rather than calling `processBlockBypassed`. <https://forum.juce.com/t/current-state-of-bypass-management/54662> · <https://www.kvraudio.com/forum/viewtopic.php?t=517967&start=30>
- `suspend()`/`resume()` are supposed to come in pairs around processing, but host behaviour varies.

**Mitigations:**
- **Keep the connection alive from a timer thread, not the audio thread.** Send keepalive/heartbeat packets at ~1 Hz independent of `processBlock`.
- **When the ring is empty, transmit silence frames rather than nothing.** The receiver keeps its clock and buffer, and there's no reconnect storm. Opus encodes digital silence to a handful of bytes, so it's nearly free.
- **Show real state in the UI:** "streaming" vs "connected, no audio from host" are different messages, and the second one prevents a support ticket.

### 5.2 Sample-rate and block-size changes mid-session

- `prepareToPlay` can be called at any time with a new rate and a new max block size. **Reset everything**: flush the ring, `reset()` every interpolator, recreate the Opus encoder (its rate is fixed at creation), and renegotiate the format with the receiver.
- Ableton's bounce-at-a-different-rate feature makes this a real, non-theoretical path. <https://forum.juce.com/t/ableton-offline-rendering-at-altered-sample-rate-issue/64024>
- Send the sample rate and channel count **in every packet header**, not just at handshake. Packets get lost; late-joining receivers exist; the rate can change under you.
- Some hosts call `prepareToPlay` repeatedly, or inconsistently. <https://forum.juce.com/t/audioprocessorgraph-preparetoplay-inconsistent-in-ableton/33893>

### 5.3 Validation: `auval` and `pluginval`

**`auval`** — mandatory gate for AU; Logic won't load a plugin that fails.

```bash
auval -a                        # list all AUs
auval -v aufx Xstm Mfrm         # validate one: type, subtype, manufacturer
auval -v aufx Xstm Mfrm -strict # what pluginval runs by default
```
<https://moonbase.sh/articles/debugging-your-audio-unit-plugin-with-auval-aka-auvaltool/>

**`pluginval`** (Tracktion, cross-platform, free) — <https://github.com/Tracktion/pluginval>

```bash
pluginval --strictness-level 10 --validate-in-process --output-dir ./logs \
          --validate "MyPlugin.vst3"
echo $?   # non-zero = failed; wire this into CI
```

Key facts:
- Strictness runs **1–10**; **5 is the accepted minimum for host compatibility**. Low levels are quick crash/coverage checks; high levels add parameter fuzzing and repeated state save/restore.
- pluginval **invokes `auval -strict` itself** at strictness ≤5, and adds **`-stress 20`** above 5 — 20 simulated seconds of multi-threaded audio I/O, specifically good at exposing race conditions. **This is the test that will find your FIFO bugs.** Run at 10.
- Only JUCE 8+ is currently supported/tested for pluginval's JUCE-integration method.

Sources: <https://melatonin.dev/blog/pluginval-is-a-plugin-devs-best-friend/> · <https://tobanteaudio.gitbook.io/juce-cookbook/testing/pluginval> · <https://forum.juce.com/t/anouncing-pluginval-an-open-source-cross-platform-plugin-validation-tool/27710>

**Validation-specific traps for a networking plugin:**
- Validators instantiate and destroy your plugin **hundreds of times in rapid succession**. Every instance creating a socket and a thread will exhaust file descriptors or hang teardown. **Lazily create the network thread only when the user enables streaming**, and make destruction fully deterministic (`stopThread(2000)`, join, close socket).
- Validators call `processBlock` at absurd block sizes (1 sample, then 8192). Your ring must survive both.
- Validators run **off the main thread and in parallel**. Anything touching a shared global (a static socket registry, a singleton discovery service) will be found.
- If your plugin blocks in the destructor waiting on a network timeout, `auval` marks it as hung and fails.

### 5.4 Multi-channel

- Get **stereo right first**; reject unsupported layouts in `isBusesLayoutSupported` rather than half-supporting them.
- AU's channel-layout negotiation is stricter than VST3's; `auval` probes many configurations, and "supports everything" is a fast way to fail.
- If you later go multichannel: Opus handles ≤2 channels per encoder instance simply, so N-channel means either `opus_multistream_encoder` or N encoder instances. PCM is far simpler for high channel counts, which is exactly why LISTENTO's 128-channel tier is lossless-only.

### 5.5 Everything else that will bite you

| Pitfall | Mitigation |
|---|---|
| Multiple instances of the plugin in one session | Bind port 0 (ephemeral) per instance; never hardcode a source port. Give each instance a UUID in the stream ID. |
| DAW project saved with streaming enabled → reopens and immediately connects | Persist the *setting* but require an explicit user action to reconnect, or at least show it loudly. |
| WiFi jitter | Bigger jitter buffer on WiFi than ethernet (SonoBus's guide is explicit). Consider auto-sizing from measured jitter. |
| Packet loss | Opus has built-in **packet loss concealment** — enable it (`OPUS_SET_PACKET_LOSS_PERC`) and use the decoder's PLC path on gaps. This is a stated reason AOO chose Opus. |
| Denormals | `juce::ScopedNoDenormals` at the top of `processBlock`, always. |
| Logging from the audio thread | Never. Use a lock-free message FIFO to a UI timer. |
| GUI reading audio state | `std::atomic` counters + a `juce::Timer` at 30 Hz. Never share a mutex with `processBlock`. |
| Unencrypted audio on the wire | LAN mode: at minimum obfuscate + authenticate with a pairing secret. Internet mode: TLS or DTLS, no exceptions — this is someone's unreleased master. |
| Battery/thermal on the phone | Opus decode is cheap; the network wake-ups are not. Batch where you can, and expect complaints if you use 2.5 ms frames. |

---

## 6. Recommended architecture for this project

```
┌─────────────────────── DAW process ───────────────────────┐
│                                                            │
│  master bus ──► [Sender plugin]                            │
│                   processBlock()                           │
│                     │  ScopedNoDenormals                   │
│                     │  ring.write()   ← lock-free, no alloc│
│                     │  pass audio through UNCHANGED        │
│                     ▼                                      │
│               AbstractFifo (≈500 ms, preallocated)         │
│                     │                                      │
│                     ▼   (network thread, Priority::high)   │
│            resample → Opus encode (10 ms frames)           │
│                     │   or PCM 24-bit for LAN-lossless     │
│                     ▼                                      │
│         [header: seq | sampleClock | rate | ch] + payload  │
└─────────────────────┬─────────────────────────────────────┘
                      │
        LAN: UDP, Bonjour-discovered      WAN: TLS/TCP :443 → relay
                      │                            │
                      └──────────┬─────────────────┘
                                 ▼
                    ┌──── Phone receiver app ────┐
                    │  jitter buffer (adjustable)│
                    │  DLL clock estimate        │
                    │  drift-correcting resample │
                    │  Opus decode + PLC         │
                    │  → CoreAudio / AAudio      │
                    │  background-audio capable  │
                    └────────────────────────────┘
```

**Build order:**
1. **v0 — LAN, UDP, PCM, no discovery.** Type the IP into the phone app. Proves the ring buffer, the threading and the OS permission story with the fewest moving parts.
2. **v1 — add Opus, a jitter buffer, and Bonjour discovery.** Ship it. Handle the macOS 15 Local Network prompt with a proper in-plugin explanation.
3. **v2 — drift correction (DLL + dynamic resample).** This is what turns "works for two minutes" into "works for a session."
4. **v3 — relay over 443** for remote listeners, if you want to compete with LISTENTO rather than Mix To Mobile.

**Reuse, don't rewrite:** read SonoBus's source and seriously evaluate vendoring **AOO** — it already implements the framing, both codecs, the jitter buffer, packet-loss handling, and the DLL-based drift correction, with a C API designed for embedding in plugins. Note the licence question: AOO's own terms are permissive-ish but check the current LICENSE; SonoBus as a *whole* is GPLv3.

---

## Appendix: all sources

**Products & prior art**
- Mix To Mobile: <https://www.production-expert.com/production-expert-1/stream-your-daw-to-your-phone-with-this-cool-software> · <https://mixdownmag.com.au/reviews/review-mix-to-mobile/> · <https://apps.apple.com/us/app/mix-to-mobile/id1659104489> · <https://soundondigital.com/sonobus-vs-mix-to-mobile/>
- LISTENTO: <https://audiomovers.com/listento> · <https://audiomovers.com/storage/pdfs/LISTENTO%20User%20Guide.pdf> · <https://www.audiomovers.com/help/faq/> · <https://www.production-expert.com/production-expert-1/audiomovers-listento-for-audio-post> · <https://tapeop.com/reviews/gear/139/listento-remote-monitoring-plug-in> · <https://www.prosoundweb.com/audiomovers-announces-new-web-transmitter-for-browser-based-collaboration/>
- SonoBus: <https://github.com/sonosaurus/sonobus> · <https://github.com/sonosaurus/sonobus/blob/main/README.md> · <https://www.sonobus.net/sonobus_userguide.pdf> · <https://www.linuxuprising.com/2021/02/sonobus-is-open-source-low-latency-peer.html>
- AOO: <https://github.com/Spacechild1/aoo> · <https://github.com/essej/aoo> · <https://aoo.iem.sh/api_documentation/aoo_v2.0-pre4/> · <https://www.soundingfuture.com/en/article/aoo-low-latency-peer-peer-audio-streaming-and-messaging> · <http://lac.linuxaudio.org/2014/papers/36.pdf>
- SoundJack / NMP: <https://www.ianhowellcountertenor.com/soundjack-real-time-online-music> · <http://www.carot.de/Docs/TMT08.pdf> · <https://arxiv.org/pdf/1808.09405>
- Cleanfeed: <https://cleanfeed.net/> · <https://blog.cleanfeed.net/commitment-to-the-highest-quality/> · <https://www.radioworld.com/tech-and-gear/cleanfeed-net-live-remotes-on-a-budget>

**Frameworks & licensing**
- VST3 MIT relicensing: <https://cdm.link/open-steinberg-vst3-and-asio/> · <https://www.kvraudio.com/news/steinberg-moves-vst-3-sdk-to-mit-open-source-license-asio-now-gplv3-65179> · <https://www.soundonsound.com/news/steinberg-adopt-mit-license-vst3> · <https://librearts.org/2025/11/steinberg-relicenses-vst3-and-asio/> · <https://steinbergmedia.github.io/vst3_dev_portal/pages/VST+3+Licensing/VST3+License.html>
- JUCE: <https://juce.com/get-juce/> · <https://juce.com/legal/juce-8-licence/> · <https://github.com/juce-framework/JUCE/blob/master/LICENSE.md> · <https://forum.juce.com/t/juce-9-is-available-now/69175> · <https://www.kvraudio.com/news/juce-9-now-available-67802> · <https://juce.com/blog/juce-roadmap-update-q3-2025/> · <https://forum.juce.com/t/juce8-license-and-open-source-projects/60987> · <https://forum.juce.com/t/revenue-limits-for-juce-tiers/61058>
- CLAP: <https://github.com/free-audio/clap> · <https://github.com/free-audio/clap-juce-extensions> · <https://cleveraudio.org/developers-getting-started/> · <https://producergrid.com/blog/clap-plugin-format-everything-you-need-to-know/>
- iPlug2: <https://github.com/iplug2/iplug2> · <https://iplug2.github.io/> · <https://iplug2-iplug2.mintlify.app/introduction> · <https://webaudioconf.github.io/papers/iplug2-desktop-plug-in-framework-meets-web-audio-modules.pdf>
- AAX/PACE: <https://www.kvraudio.com/forum/viewtopic.php?t=479249> · <https://forum.hise.audio/topic/8190/how-i-got-my-plugin-codesigned-for-aax> · <https://note.com/kawato3/n/ne11473420ad5?hl=en>
- 2026 overview: <https://www.youngju.dev/blog/culture/2026-05-16-audio-plugin-development-2026-juce-8-vst3-au-aax-clap-iplug2-faust-cmajor-elementary-audio-deep-dive.en>

**JUCE mechanics**
- <https://docs.juce.com/master/classAbstractFifo.html> · <https://docs.juce.com/master/classAudioProcessor.html> · <https://docs.juce.com/master/classDatagramSocket.html> · <https://docs.juce.com/master/classStreamingSocket.html> · <https://docs.juce.com/master/classGenericInterpolator.html> · <https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_basics/utilities/juce_Interpolators.h>
- <https://forum.juce.com/t/reading-writing-values-lock-free-to-from-processblock/50947> · <https://forum.juce.com/t/audioprocessor-processblock-and-thread-notify-is-it-lock-free/27048> · <https://forum.juce.com/t/preparetoplay-and-processblock-thread-safety/32193> · <https://forum.juce.com/t/exc-bad-access-when-pushing-to-abstractfifo/42046> · <https://forum.juce.com/t/lagrangeinterpolator-for-realtime-resampling/20054> · <https://github.com/ncblair/NTHN_TEMPLATE_PLUGIN>

**Codec**
- <https://wiki.hydrogenaudio.org/index.php?title=Opus> · <https://arxiv.org/pdf/1602.04845> · <https://arxiv.org/pdf/1602.05311>

**Sandboxing, entitlements, OS policy**
- <https://developer.apple.com/library/archive/technotes/tn2312/_index.html> · <https://lapcatsoftware.com/articles/hardened-runtime-sandboxing.html> · <https://developer.apple.com/forums/thread/698926> · <https://developer.apple.com/forums/thread/679409> · <https://forums.macrumors.com/threads/native-m1-plugins-are-not-handled-natively-in-logic-pro-auhostingservice.2326554/>
- Local Network privacy: <https://eclecticlight.co/2025/03/10/manage-privacy-protection-for-network-devices-and-others/> · <https://eclecticlight.co/2026/01/18/last-week-on-my-mac-local-network-privacy-revealed/> · <https://mjtsai.com/blog/2024/10/02/local-network-privacy-on-sequoia/> · <https://support.native-instruments.com/hc/en-us/articles/27553634808861> · <https://developer.apple.com/forums/thread/792453> · <https://developer.apple.com/forums/thread/766270> · <https://developer.apple.com/forums/thread/760964>
- iOS: <https://developer.apple.com/forums/thread/790307> · <https://developer.apple.com/forums/thread/741503> · <https://developer.apple.com/forums/thread/664264> · <https://forum.juce.com/t/bonjour-on-ios-nslocalnetworkusagedescription/56413> · <https://ptkd.com/journal/ios-local-network-privacy-permission>
- Signing/notarization: <https://melatonin.dev/blog/how-to-code-sign-and-notarize-macos-audio-plugins-in-ci/> · <https://www.kvraudio.com/forum/viewtopic.php?t=531663> · <https://moonbase.sh/articles/code-signing-audio-plugins-in-2025-a-round-up/> · <https://forum.juce.com/t/prepare-plugins-for-distribution-on-macos-notarization-code-signing-etc/53124>

**Validation & DAW behaviour**
- <https://github.com/Tracktion/pluginval> · <https://melatonin.dev/blog/pluginval-is-a-plugin-devs-best-friend/> · <https://tobanteaudio.gitbook.io/juce-cookbook/testing/pluginval> · <https://moonbase.sh/articles/debugging-your-audio-unit-plugin-with-auval-aka-auvaltool/> · <https://forum.juce.com/t/anouncing-pluginval-an-open-source-cross-platform-plugin-validation-tool/27710>
- <https://developer.apple.com/forums/thread/674704> · <https://forum.juce.com/t/logic-pro-process-block-calls-for-synthesizer/53696> · <https://forum.juce.com/t/preview-audio-in-logic-or-pro-tools-when-transport-stopped/56454> · <https://forum.juce.com/t/current-state-of-bypass-management/54662> · <https://www.kvraudio.com/forum/viewtopic.php?t=517967&start=30> · <https://forum.juce.com/t/ableton-offline-rendering-at-altered-sample-rate-issue/64024> · <https://forum.juce.com/t/studio-one-isnonrealtime-always-false-in-preparetoplay/24730> · <https://forum.juce.com/t/trouble-with-isnonrealtime-for-offline-render/18564> · <https://forum.juce.com/t/audioprocessorgraph-preparetoplay-inconsistent-in-ableton/33893>

**WebRTC option**
- <https://github.com/paullouisageneau/libdatachannel> · <https://libdatachannel.org/> · <https://tensorworks.com.au/blog/a-brief-comparison-of-libdatachannel-and-libwebrtc/>
