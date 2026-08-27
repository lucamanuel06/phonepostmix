# 02 — Transport & Codecs for DAW-plugin → Phone Real-Time Audio

**Scope.** A desktop DAW plugin (C++, presumably JUCE/VST3/AU) must stream its master bus to a phone
**web browser** on the same Wi-Fi, and optionally over the internet. The listener is judging a **mix** —
so fidelity is a first-class requirement, not an afterthought. This document compares transports,
codecs, discovery mechanisms, clock/jitter handling, and C++ library choices.

Research date: **2026-08-27**. Browser-support claims are as of that date.

---

## 0. TL;DR — the recommendation

| Layer | LAN (primary) | Internet (secondary) |
|---|---|---|
| Transport | **WebRTC** (`RTCPeerConnection`, audio track) or **WebTransport datagrams** | **WebRTC** with STUN + TURN fallback |
| Codec | **Opus 510 kbps stereo, `application=audio`, 10 ms frames**, plus an optional **24-bit PCM / FLAC "reference mode"** | Opus 128–256 kbps, 20 ms frames |
| Discovery | QR code encoding the full HTTPS URL incl. IP + token | account/relay |
| Serving the page | **HTTPS with a real, publicly-trusted cert for a name that resolves to the LAN IP** (Plex/`nip.io` pattern) | normal HTTPS |

**The single most important finding:** `http://192.168.x.x:8080` is **NOT a secure context**. This kills
`AudioWorklet`, `WebCodecs`, `WebTransport`, `getUserMedia`, service workers, and `SharedArrayBuffer`.
Only `127.0.0.1/8`, `::1`, `localhost`, and `*.localhost` are exempt — and the phone is not the loopback
device. See **§5.3**, which is the section that constrains the entire architecture.

---

## 1. Transport options compared

### 1.1 The latency budget

Before comparing protocols, fix the budget. End-to-end mouth-to-ear for "producer nods along on the couch"
is comfortable at 100–250 ms; for "engineer A/Bs while B tweaks a fader in real time" you want < 100 ms.
You are *not* building a jam-along tool (that needs < 25 ms and is impossible through a phone browser).

Contributions on a LAN:

| Stage | Typical |
|---|---|
| DAW buffer (host block, 128–512 frames @ 48 k) | 2.7–10.7 ms |
| Encode (Opus 10 ms frame + lookahead) | ~12.5 ms |
| Packetize + host network stack | ~1 ms |
| Wi-Fi 5 GHz one-way, p50 / p90 / p99 | ~2 ms / ~5 ms / **~80 ms** |
| Jitter buffer (must cover the p99 tail) | 20–60 ms |
| Decode | ~1 ms |
| Browser output graph (`AudioContext.outputLatency`) | 10–40 ms (Android often worse; 100+ ms on some devices) |
| Phone DAC + Bluetooth if used | 5 ms wired / **120–200 ms Bluetooth** |

Note the two dominators you do not control: the **Wi-Fi p99 tail** (measurements show ~5 ms for p90 but
needing ~80 ms to guarantee 99 % delivery — see the [HPBN Wi-Fi chapter](https://hpbn.co/wifi/)) and the
**phone's own output latency**. Warn users off Bluetooth in the UI; it dwarfs everything else you optimise.

### 1.2 WebRTC (`RTCPeerConnection` audio track)

**Latency.** Sub-second is the industry baseline; 200–500 ms is quoted glass-to-glass for *video*
([Fora Soft](https://www.forasoft.com/blog/article/webrtc-video-steaming-app-vs-hls)). For audio-only on a
LAN with a tuned jitter buffer you realistically land at **40–90 ms** end-to-end. Chrome's NetEq adapts
its target automatically; you can bias it:

```js
const receiver = pc.getReceivers().find(r => r.track.kind === 'audio');
// Standardised name (Chrome 114+, Firefox). Seconds.
if ('jitterBufferTarget' in receiver) receiver.jitterBufferTarget = 60; // ms in the shipped impl
// Legacy Chrome name, seconds:
if ('playoutDelayHint' in receiver) receiver.playoutDelayHint = 0.06;
```

`jitterBufferTarget` is **not Baseline** — Chrome M79+ (as `playoutDelayHint`) and Firefox have it, Safari
does not. Feature-detect; on Safari you get NetEq's default (~50–80 ms for audio).
Refs: [MDN `jitterBufferTarget`](https://developer.mozilla.org/en-US/docs/Web/API/RTCRtpReceiver/jitterBufferTarget),
[Chrome intent-to-ship](https://groups.google.com/a/chromium.org/g/blink-dev/c/4W4orKqA3Rs),
[Bugzilla 1592988](https://bugzilla.mozilla.org/show_bug.cgi?id=1592988).

**NAT/firewall.** Best in class. ICE + STUN gets a direct path in ~80 % of internet cases; TURN relays the
rest. On a LAN, host candidates connect immediately — *but see the mDNS candidate wrinkle in §3.4*.

**Browser support 2026.** Universal. iOS Safari included. The Encoded Transform API
(`RTCRtpScriptTransform`) reached **cross-browser Baseline in 2025** — Safari shipped it in 2022,
Firefox 117, Chrome later — so you can intercept encoded frames in JS if you want to shove your own
payload (e.g. FLAC) through an SRTP audio track.
Ref: [Bugzilla 1631263](https://bugzilla.mozilla.org/show_bug.cgi?id=1631263).

**Gotchas.**
- Chrome historically **downmixes Opus to mono** unless `stereo=1` *and* `sprop-stereo=1` are munged into
  **both** local and remote SDP. See [webrtc issue 41481053](https://issues.webrtc.org/issues/41481053).
- The audio pipeline may apply AGC/AEC/NS. For a *sending* native peer you control the encoder directly;
  for anything that touches `getUserMedia` you must disable them explicitly.
- iOS Safari **suspends WebRTC and Web Audio when the screen locks or Safari backgrounds**
  ([Apple forum thread 774239](https://developer.apple.com/forums/thread/774239)). There is no workaround
  from a web page. This is a hard product constraint: the listener must keep the screen on.

**SDP munging for high-fidelity music:**

```
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1;maxaveragebitrate=510000;maxplaybackrate=48000;cbr=0;usedtx=0
```

### 1.3 WebRTC data channel (raw, unreliable)

Skip SRTP/Opus entirely: send your own PCM/FLAC frames over an unreliable, unordered data channel.

```js
const dc = pc.createDataChannel('audio', {
  ordered: false,
  maxRetransmits: 0,      // pure unreliable — never use maxPacketLifeTime AND maxRetransmits together
});
dc.binaryType = 'arraybuffer';
```

**Pros:** total control of codec, packet size, and buffering; bypasses NetEq entirely; works everywhere
WebRTC works, including iOS Safari. **Cons:** you write the jitter buffer, PLC, and drift correction
yourself (see §4), and you need `AudioWorklet` to play it out — which needs a **secure context** (§5.3).
SCTP over DTLS adds a few hundred µs, negligible.

This is the option that lets you do **lossless over the LAN and Opus over the internet on the same
transport**, which is architecturally very attractive.

### 1.4 Plain UDP / RTP

**Latency:** the floor. LAN RTT under 2 ms, no framing overhead beyond the 12-byte RTP header.
**Browser support: zero.** A web page cannot open a UDP socket. This is only viable if the phone side is a
**native app**. If you ever ship a native iOS/Android client, plain RTP with your own header is the right
answer and is what SonoBus effectively does (it uses a forked Audio-over-OSC over UDP with sequence
numbers — see the [SonoBus user guide](https://sonobus.net/sonobus_userguide.html)).

**NAT:** you must implement your own hole punching. On a LAN it's free.

### 1.5 RTSP

Control protocol (RTSP) + RTP media. **No browser support at all** — no browser has ever shipped an RTSP
client. Requires a gateway to WebRTC/HLS, which just moves the problem. **Rule out.**

### 1.6 HLS / LL-HLS

Segment-based. Classic HLS is 6–30 s. Apple's LL-HLS with partial segments gets to ~2 s typical; heroic
tuning reaches **~900 ms glass-to-glass** ([WINK experiments](https://www.wink.co/documentation/Ultra-Low-Latency-HLS-Experiments-2025)).

- **Pros:** works natively in iOS Safari via `<video src="...m3u8">`; trivially firewall-friendly (plain
  HTTPS); scales to many listeners via CDN.
- **Cons:** ~1 s at absolute best is 10× your budget; segment encoding adds encoder delay; you cannot
  carry PCM.
- **Verdict:** wrong tool for real-time mix referencing. Possibly interesting as a *"share a listen link
  with 50 clients"* secondary feature, never as the primary path.

### 1.7 WebSocket + Web Audio

Send binary frames over `wss://`, decode in JS, play through an `AudioWorklet` ring buffer.

**Latency:** on a quiet LAN, **30–80 ms** achievable. The problem is TCP: a single lost packet stalls the
whole stream (head-of-line blocking) and TCP retransmit + RTO on Wi-Fi can spike to hundreds of ms.
Wi-Fi already retransmits at L2, so losses are rare but bursty; when they happen you get an audible gap
*and* a buffer-level step that your drift controller then has to unwind.

**Pros:** by far the simplest thing to implement from C++ (see §5.4 — uWebSockets/cpp-httplib give you a
WS server in ~30 lines), no ICE, no DTLS, no SDP. **Cons:** TCP; and `wss://` from an HTTPS page means you
need TLS anyway (mixed content blocks `ws://` from an `https://` page).

**Verdict:** the correct **v1** for a LAN-only MVP if — and only if — you have already solved HTTPS (§5.3),
because you need `AudioWorklet` regardless. Then upgrade the transport to WebRTC data channel later
without touching the playback side.

### 1.8 SRT

Secure Reliable Transport: UDP + ARQ with a configurable latency window. Excellent for contribution links
(default 120 ms latency window, tunable down to ~4× RTT). Mature C++ library
([Haivision/srt](https://github.com/Haivision/srt), MPL-2.0), easy CMake build.

**Browser support: zero.** Same fate as RTSP — needs a gateway. Only relevant if you build a native
receiver, and even then WebRTC data channels get you the same result with browser reach thrown in.

### 1.9 QUIC / WebTransport

**This became viable in 2026.** Safari 26.4 (March 2026, macOS + iOS + iPadOS) shipped WebTransport,
making it **Baseline** — Chrome, Edge, and Firefox already had it.
Refs: [WebKit features for Safari 26.4](https://webkit.org/blog/17862/webkit-features-for-safari-26-4/),
[webrtc.ventures: WebTransport is now Baseline](https://webrtc.ventures/2026/04/webtransport-is-now-baseline-what-it-means-for-real-time-media/).

```js
const wt = new WebTransport('https://192-168-1-50.your-lan-domain.dev:4433/audio', {
  // Escape hatch for LAN certs — see §5.3
  serverCertificateHashes: [{ algorithm: 'sha-256', value: hashBytes }],
});
await wt.ready;
const reader = wt.datagrams.readable.getReader();
for (;;) {
  const { value, done } = await reader.read();   // value: Uint8Array, unreliable, unordered
  if (done) break;
  ringBuffer.push(value);
}
```

**Latency:** comparable to WebRTC data channels for datagrams — QUIC over UDP, no HoL blocking across
streams, and datagrams have no reliability at all. Slightly *better* than SCTP-over-DTLS in setup time
(1-RTT handshake, no ICE round trips).

**`serverCertificateHashes`** is the killer feature for this project: it lets a page connect to a server
whose certificate is *not* Web-PKI-trusted, by pinning the SHA-256 of the cert. Explicitly designed for
"hosts that are not publicly routable". Constraints: `allowPooling: false`, and browsers enforce a short
max validity (Chrome: **14 days**) on such certificates, so you must rotate. Refs:
[Chromium intent-to-ship](https://groups.google.com/a/chromium.org/g/blink-dev/c/m0v9XiwKA4M/m/GtMq9j_iAAAJ),
[MDN WebTransport()](https://developer.mozilla.org/docs/Web/API/WebTransport/WebTransport).

**Caveat that spoils it:** `serverCertificateHashes` solves the *WebTransport* trust problem but **not the
page-origin problem**. The page itself still has to be served from a secure context to call
`new WebTransport()` at all. So it doesn't rescue you from §5.3.

**NAT/firewall:** UDP/443 is usually open; some corporate networks block QUIC and there is no TCP
fallback in WebTransport itself.

**C++ server side:** the weak point. You need an HTTP/3 server: [msquic](https://github.com/microsoft/msquic)
(MIT, good Windows/macOS support) or [quiche](https://github.com/cloudflare/quiche) (Rust, C API), plus
your own WebTransport session layer. Materially more work than libdatachannel.

### 1.10 Summary table

| Transport | LAN latency (audio, realistic) | Phone browser 2026 | NAT/firewall | C++ effort | Lossless capable |
|---|---|---|---|---|---|
| WebRTC audio track (Opus) | 40–90 ms | ✅ all | ✅ ICE/STUN/TURN | Medium (libdatachannel) | ❌ (Opus only in practice) |
| WebRTC data channel | 30–70 ms | ✅ all | ✅ | Medium | ✅ |
| WebTransport datagrams | 30–70 ms | ✅ Baseline since Safari 26.4 | ✅ UDP/443, no TCP fallback | **High** (HTTP/3 stack) | ✅ |
| WebSocket (`wss`) | 30–80 ms, bursty on loss | ✅ all | ✅ TCP/443 | **Low** | ✅ |
| Plain UDP/RTP | 5–20 ms | ❌ | manual | Low | ✅ |
| SRT | 20–150 ms | ❌ | ✅ | Low | ✅ |
| RTSP | n/a | ❌ | ❌ | — | — |
| LL-HLS | 900 ms – 3 s | ✅ | ✅ | Medium | ❌ |

---

## 2. Codecs

### 2.1 The fidelity argument

Mix referencing is a **critical-listening** task: the whole point is to hear the top octave, the reverb
tails, the stereo image, and the level of the 2 kHz sibilance. A codec that is "transparent" in an ABX
test on a laptop is not necessarily transparent when someone is specifically hunting for the artefact you
introduced. For a LAN — where you have 1 Gb/s of headroom and 2 ms of RTT — there is **no reason to lose
information at all**.

Bandwidth math, 48 kHz stereo:

| Format | Bit rate | Fits 802.11ac? |
|---|---|---|
| 32-bit float PCM | 3.072 Mbit/s | trivially |
| 24-bit PCM | **2.304 Mbit/s** | trivially |
| 16-bit PCM | 1.536 Mbit/s | trivially |
| FLAC (24/48, ~55 % ratio) | ~1.3 Mbit/s | trivially |
| Opus 510 kbps | 0.51 Mbit/s | trivially |
| Opus 256 kbps | 0.256 Mbit/s | fine on 4G |
| 96 kHz / 24-bit PCM | 4.608 Mbit/s | fine |

Even the worst case is a rounding error on a modern AP. **Ship raw PCM as the LAN default.**

### 2.2 Raw PCM over LAN — the reference path

Choose the packet payload so one audio packet fits one MTU (1500 − 20 IP − 8 UDP − 12 RTP ≈ **1452 B**;
over DTLS/SCTP budget ~1200 B to be safe).

| Frames | Duration @48k | 24-bit stereo bytes | Fits ≤1200 B? |
|---|---|---|---|
| 64 | 1.33 ms | 384 | ✅ |
| **128** | **2.67 ms** | **768** | ✅ |
| 192 | 4.0 ms | 1152 | ✅ (tight) |
| 256 | 5.33 ms | 1536 | ❌ — fragments |

**Use 128 frames.** That is 375 packets/s per direction — fine. Add a tiny header:

```cpp
#pragma pack(push, 1)
struct AudioPacketHeader {
    uint32_t magic;        // 'PPMX'
    uint32_t seq;          // packet sequence, for reorder + loss detection
    uint64_t sampleTime;   // absolute frame index since stream start (the DAW clock)
    uint32_t sampleRate;   // 44100 / 48000 / 96000
    uint8_t  channels;     // 2
    uint8_t  format;       // 0=pcm16, 1=pcm24, 2=pcmf32, 3=flac, 4=opus
    uint16_t frames;       // 128
};                         // 24 bytes
#pragma pack(pop)
```

`sampleTime` is what makes drift correction tractable (§4) — never derive timing from arrival time alone.

**Browser side:** the payload arrives as an `ArrayBuffer`; convert to float and push to a
`SharedArrayBuffer` ring the `AudioWorklet` reads. Note `SharedArrayBuffer` needs
`crossOriginIsolated` (COOP+COEP headers) *and* a secure context — you must send:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

If you'd rather avoid COOP/COEP, use `port.postMessage(buf, [buf])` transfers into the worklet instead;
slightly more GC pressure but works without cross-origin isolation.

### 2.3 FLAC

- **Lossless**, so fidelity is identical to PCM by definition.
- Compression ~40–60 % on typical mastered material. On a LAN you don't need it; on a **good** internet
  link (≥ 3 Mbit/s up) FLAC is a genuinely interesting "lossless over WAN" mode that Audiomovers-style
  products advertise.
- **Latency:** FLAC is block-based with no lookahead beyond the block. Block size is anything from 16 to
  65535 samples ([RFC 9639](https://www.rfc-editor.org/info/rfc9639/)). A 1024-sample block at 48 k is
  21 ms; a **256-sample block is 5.3 ms** and still compresses usefully (a few % worse ratio). Decode is
  integer-only and very cheap ([Xiph FLAC features](https://xiph.org/flac/features.html)).
- **Browser decode:** no native streaming FLAC decoder you can drive frame-by-frame. `<audio>` plays FLAC
  files, which is useless here. You need a **WASM decoder** (libFLAC compiled with Emscripten, ~100 KB) in
  an `AudioWorklet` or Worker. Adds build complexity; decode cost is trivial.
- **Verdict:** worth it only for the WAN lossless mode. On LAN, raw PCM is simpler and has zero codec
  latency.

### 2.4 Opus

The best lossy option, by a wide margin, and the only one with **native browser decode via WebCodecs**.

**Bitrate.** Range 6–510 kbps stereo; the music "sweet spot" is quoted as 40–100 kbps
([Hydrogenaudio](https://wiki.hydrogenaudio.org/index.php?title=Opus)). For *mix judging* ignore the sweet
spot and run at **256–510 kbps**, where Opus is effectively indistinguishable on program material. Do not
run below 192 kbps for this use case: the reason is not "audibility of artefacts in general" but that
Opus's coupled-stereo and band-folding decisions at lower rates alter exactly the things (stereo width,
top-octave air) that the listener is being asked to evaluate.

**Frame size and delay.** Frames of 2.5, 5, 10, 20, 40, 60 ms. Algorithmic delay:

| Config | Algorithmic delay |
|---|---|
| `AUDIO`/`VOIP`, 20 ms frame | 26.5 ms (20 ms frame + 6.5 ms lookahead) |
| `AUDIO`, 10 ms frame | 16.5 ms |
| `RESTRICTED_LOWDELAY`, 10 ms | 12.5 ms (SILK off ⇒ −4 ms sync delay) |
| `RESTRICTED_LOWDELAY`, 5 ms | 7.5 ms |
| `RESTRICTED_LOWDELAY`, 2.5 ms | **~5 ms** |

Opus "scales to delays as low as 5 ms, even lower than AAC-ELD (15 ms)" —
[Valin et al., *High-Quality, Low-Delay Music Coding in the Opus Codec*](https://arxiv.org/pdf/1602.04845).

**`application=audio` vs `restricted_lowdelay`.**
- `OPUS_APPLICATION_AUDIO`: "favour faithfulness to the original input" — the hybrid SILK+CELT machinery
  is available, and this is the tuning you want for **music quality**.
- `OPUS_APPLICATION_RESTRICTED_LOWDELAY`: forces CELT-only, disabling SILK to shave 4 ms and allowing
  2.5/5 ms frames.

At the bitrates you care about (≥192 kbps) Opus is running in CELT mode anyway, so the *quality*
difference between the two is small — but `AUDIO` keeps the encoder free to make better decisions and is
the safer default for fidelity. **Recommendation: `OPUS_APPLICATION_AUDIO` with 10 ms frames** —
16.5 ms of codec delay is not the bottleneck in a budget where Wi-Fi p99 is 80 ms, and you keep the
quality-tuned code path. Reserve `RESTRICTED_LOWDELAY` for an explicit "ultra-low-latency, quality
secondary" mode.

**Encoder setup:**

```cpp
int err = 0;
OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);

opus_encoder_ctl(enc, OPUS_SET_BITRATE(510000));            // max useful stereo rate
opus_encoder_ctl(enc, OPUS_SET_VBR(1));                     // VBR: better quality per bit
opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(0));          // unconstrained VBR
opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));             // full quality; cheap on desktop
opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(2));          // never downmix to mono
opus_encoder_ctl(enc, OPUS_SET_DTX(0));                     // never gate silence — silence is information
opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));

// NEVER hard-code the delay — ask:
opus_int32 lookahead = 0;
opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));       // samples @48k

// 10 ms frame = 480 frames per channel
const int FRAME = 480;
unsigned char out[1500];
int n = opus_encode_float(enc, interleavedFloat, FRAME, out, sizeof(out));
```

(`OPUS_GET_LOOKAHEAD` — the docs explicitly say "applications needing delay compensation should call this
CTL rather than hard-coding a value":
[Opus encoder CTLs](https://opus-codec.org/docs/opus_api-1.5/group__opus__encoderctls.html).)

**Turn DTX off.** A mix has meaningful near-silence (fades, tails, pre-roll). DTX will eat it.

### 2.5 Opus in the browser via WebCodecs `AudioDecoder`

This is how you decode Opus yourself when you're not using an SRTP audio track (i.e. over a data channel,
WebTransport, or WebSocket).

```js
const dec = new AudioDecoder({
  output: (audioData) => {           // AudioData, planar or interleaved f32
    const ch0 = new Float32Array(audioData.numberOfFrames);
    const ch1 = new Float32Array(audioData.numberOfFrames);
    audioData.copyTo(ch0, { planeIndex: 0, format: 'f32-planar' });
    audioData.copyTo(ch1, { planeIndex: 1, format: 'f32-planar' });
    ring.write(ch0, ch1);            // hand to the AudioWorklet
    audioData.close();               // MUST close or you leak
  },
  error: (e) => console.error(e),
});

dec.configure({
  codec: 'opus',
  sampleRate: 48000,
  numberOfChannels: 2,
  description: opusHeadBytes,        // OpusHead / ID header; optional for raw packets
});

dec.decode(new EncodedAudioChunk({
  type: 'key',                       // every Opus packet is a key frame
  timestamp: microseconds,           // derived from your header's sampleTime
  data: packetBytes,
}));
```

**Support, 2026:**
- **Chrome / Edge:** full WebCodecs incl. `AudioDecoder` since 94.
- **Firefox:** 130+ on desktop. **Firefox for Android: no WebCodecs at all.**
- **Safari:** WebCodecs landed 16.4 but **video-only through 18.7** — `AudioDecoder`, `AudioEncoder`, and
  `EncodedAudioChunk` were `undefined`. **Audio works from Safari 26.0** (macOS/iOS/iPadOS).
  Refs: [MDN Codec selection](https://developer.mozilla.org/en-US/docs/Web/API/WebCodecs_API/Codec_selection),
  [webcodecsfundamentals.org](https://webcodecsfundamentals.org/audio/encoded-audio-chunk/).

So: **feature-detect `window.AudioDecoder`**, and keep a WASM `libopus` decoder as the fallback for older
iOS and Firefox Android. Also note there is **no direct bridge from `AudioData` to Web Audio** — you must
`copyTo()` into `Float32Array`s and feed a worklet yourself.

**Requires a secure context** (WebCodecs is secure-context-gated). Again: §5.3.

### 2.6 AAC-LD / AAC-ELD

- AAC-LD: designed for two-way communication, near-transparent, ~20 ms delay at 48 k.
- AAC-ELD: overall algorithmic delay ~15 ms in the low-delay configuration; the dual-rate SBR variant
  costs ~30–31 ms ([Wikipedia AAC-LD](https://en.wikipedia.org/wiki/AAC-LD),
  [Comrex algorithm chart](https://www.comrex.com/resources/algorithm-chart/)).
- **Licensing:** patent-encumbered (Via LA / Fraunhofer). You would need a licence to ship.
- **Browser decode:** WebCodecs supports `mp4a.40.2` (AAC-LC) reasonably well, but AAC-LD/ELD
  (`mp4a.40.23` / `mp4a.40.39`) are **not** decodable in browsers.
- **Verdict: rule out.** Opus beats it on delay, on licence cost, and on browser support. The only reason
  Audiomovers offers AAC is legacy client compatibility — and their own docs note AAC's
  "encoders/decoders are more processor-heavy than OPUS and may require higher latency"
  ([Audiomovers quality settings](https://ftp.audiomovers.com/what-audio-quality-settings-are-right-for-you/)).

### 2.7 Codec recommendation

Mirror what Audiomovers LISTENTO does, because it's the right shape: offer **PCM 16/24/32-bit lossless**
plus **Opus at 128/256 kbps** and let the user pick, with the latency budget shown in the UI
([LISTENTO](https://audiomovers.com/listento)).

| Mode | Codec | Frame | Bit rate | Use |
|---|---|---|---|---|
| **Reference (LAN)** | 24-bit PCM | 128 frames (2.67 ms) | 2.3 Mbit/s | mix judging — the default |
| Lossless WAN | FLAC, 256-sample blocks | 5.3 ms | ~1.3 Mbit/s | good uplink |
| High-quality WAN | Opus `AUDIO` | 10 ms | 256–510 kbps | normal internet |
| Low-bandwidth | Opus `AUDIO` | 20 ms | 96–128 kbps | 4G / hotel Wi-Fi |

---

## 3. Discovery on a LAN

### 3.1 mDNS / DNS-SD from C++ — works well

Advertising a `_phonepostmix._tcp.local` service from the plugin is straightforward and cross-platform:

| Option | Notes |
|---|---|
| **Apple Bonjour** (`dns_sd.h`) | Built into macOS; on Windows requires the Bonjour service (ships with iTunes — *do not rely on it*) |
| **[mdns](https://github.com/mjansson/mdns)** (mjansson) | Single-header C, public domain, zero deps. **Best fit for a plugin** — no install footprint |
| **[mDNSResponder](https://github.com/apple-oss-distributions/mDNSResponder)** | Apple's, buildable on Windows, heavyweight |
| **Avahi** | Linux only |

```cpp
// Conceptual DNS-SD record set you want to publish:
//   Service:  _phonepostmix._tcp.local
//   Instance: "Luca's Studio (Ableton)._phonepostmix._tcp.local"
//   Host:     phonepostmix-a1b2.local  -> 192.168.1.50
//   Port:     8443
//   TXT:      v=1 proto=wrtc,ws fmt=pcm24,opus sr=48000 tok=<pairing-token>
```

### 3.2 mDNS from a browser — **does not work. Confirmed.**

This is worth being unambiguous about, because it is the discovery question people get wrong.

**A web page cannot perform an mDNS query.** JavaScript has no UDP socket API and no multicast API. There
is no `navigator.discoverServices()`. `chrome.mdns` exists but is a **Chrome Apps/Extensions API**, not
available to web pages (and Chrome Apps are dead).

What *can* happen is that the browser hands a `.local` name to the **OS resolver**, and the OS may resolve
it via mDNS. The Chromium team's own statement: Chrome "will always delegate host resolution to the OS to
allow it to be resolved via mDNS **if and only if that is the OS's resolution behavior**"
([chromium-discuss](https://groups.google.com/a/chromium.org/g/chromium-discuss/c/6b0vVreNTvQ)).

Per-platform reality:

| Phone platform | `http://name.local` in the browser |
|---|---|
| **iOS / iPadOS Safari** | ✅ works — Bonjour/mDNSResponder is built into iOS |
| **Android Chrome** | ❌ **generally fails.** Android has no system-wide mDNS resolver exposed to `getaddrinfo`; apps must use `NsdManager`, which Chrome does not wire into host resolution |

So `.local` names are a **50 % solution** — they work for iPhone users and silently fail for Android
users. You cannot build discovery on them.

Even where it resolves, it doesn't help with HTTPS: a `.local` name cannot get a publicly-trusted
certificate (CAs will not issue for reserved TLDs), so you'd be back to a cert warning.

### 3.3 What actually works: QR code pairing

**This is the answer.** The desktop plugin already knows its own IP; render a QR code containing the full
URL, and the user points their phone camera at their monitor. Both iOS Camera and Android's camera/Google
Lens open URLs from QR natively — no app install, no typing, no discovery protocol.

```
https://192-168-1-50.lan.yourdomain.dev:8443/#t=8f3a...c91d
        └────────┬────────┘                    └────┬───┘
         name resolving to the LAN IP           pairing token in the
         with a real, trusted certificate       fragment (never sent to a server)
```

Design notes:
- Put the token in the **fragment** (`#`) so it never appears in a server log or `Referer`.
- Keep the token short-lived and single-use; the WebSocket/WebRTC handshake exchanges it for a session.
- Show the plain URL underneath the QR as a fallback for manual entry.
- If multiple interfaces exist (Ethernet + Wi-Fi + VPN + Docker bridges), let the user pick, and default
  to the one on the same subnet as the default route. VPN adapters are the #1 cause of "the QR points at
  the wrong IP" bug reports.
- Regenerate the QR if the IP changes (DHCP lease, network switch).

C++ QR generation: [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator) (MIT, single
file, no deps) or [qrcodegen](https://github.com/ricmoo/QRCode). Render into a JUCE `Image`.

### 3.4 WebRTC's own mDNS wrinkle

Unrelated to discovery but relevant: browsers **obfuscate local IP addresses in ICE candidates** by
replacing them with ephemeral `xxxx.local` mDNS names
([draft-ietf-mmusic-mdns-ice-candidates](https://datatracker.ietf.org/doc/html/draft-ietf-mmusic-mdns-ice-candidates-03)).

So if the **browser is the offerer** and your C++ app has to connect to a browser host candidate, your app
must be able to **resolve `.local` names via mDNS** to use that candidate. Two mitigations:
1. Have the **C++ side gather the host candidates** (it's the "server"; the browser connects to its
   candidate, which is a plain IP and is not obfuscated). This is the normal shape anyway.
2. Implement mDNS resolution in the native peer. libdatachannel's ICE agent (libjuice) handles plain IPs;
   `.local` resolution depends on the OS.

Also: Chrome's **Local Network Access** permission (Chrome 141/142, Sept 2025+) will eventually gate
WebRTC candidates pointing at local addresses. Today WebRTC/WebSocket/WebTransport are explicitly listed
as **not yet covered**, but this is on the roadmap — see §5.3.

### 3.5 Discovery recommendation

1. **QR code (primary)** — 95 % of users, zero friction.
2. **Manual URL entry (fallback)** — big, readable, in the plugin UI.
3. **mDNS advertisement (nice-to-have)** — publish `_phonepostmix._tcp`, useful for a future **native**
   companion app and for desktop-to-desktop. Do not build the phone-browser flow on it.

---

## 4. Clock drift, jitter buffers, adaptive buffering

### 4.1 The problem

The DAW's audio clock (the interface's crystal) and the phone's DAC clock are free-running and unrelated.
A typical consumer crystal is ±50 ppm; two of them can differ by **100 ppm ≈ 4.8 samples/second at 48 kHz
≈ 0.36 s of accumulated offset per hour**. Left uncorrected you get a buffer that monotonically fills
(→ growing latency, then overflow) or drains (→ underrun/glitch every few minutes).

There is a second, nastier version: **the browser's `AudioContext` may not run at 48 kHz at all.** iOS
historically prefers 44.1 kHz and there are long-standing bugs when a 48 kHz context is forced
([godot#36643](https://github.com/godotengine/godot/issues/36643),
[WebKit 154538](https://bugs.webkit.org/show_bug.cgi?id=154538)). Read
`audioCtx.sampleRate` and treat it as ground truth — never assume.

```js
// Don't request a rate; take what the device gives and resample to it.
const ctx = new AudioContext({ latencyHint: 'interactive' });
console.log(ctx.sampleRate);       // 48000 on most Android, 44100 on many iPhones
console.log(ctx.baseLatency, ctx.outputLatency);
```

### 4.2 The measurement

Never use packet arrival times — they're dominated by network jitter. Use **buffer fill level**, which is
the integral of the rate difference and therefore a clean, low-noise drift estimate:

```
fillLevel(t)  = writePtr − readPtr    (frames in the ring)
drift_ppm    ≈ 1e6 · d(fillLevel)/dt / sampleRate
```

Low-pass this hard (time constant of **10–60 s**) — you are estimating a physical constant, and
over-reacting to short-term jitter causes audible pitch wobble.

### 4.3 The correction: asynchronous sample rate conversion

Run a resampler on the receive side whose ratio is continuously nudged by a slow PI controller:

```js
// AudioWorkletProcessor, conceptual
class StreamProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ratio = 1.0;                  // input frames consumed per output frame
    this.target = TARGET_FILL;         // e.g. 0.040 * sampleRate frames
    this.integral = 0;
  }
  process(_inputs, outputs) {
    const fill = this.ring.available();
    const err  = (fill - this.target) / this.target;

    // PI controller, deliberately sluggish
    this.integral += err * (128 / sampleRate);
    const kp = 2e-5, ki = 1e-6;
    let r = 1.0 + kp * err + ki * this.integral;

    // Clamp: >0.5% pitch change is audible on sustained material
    this.ratio = Math.max(0.998, Math.min(1.002, r));

    this.resampler.process(this.ring, outputs[0], this.ratio);
    return true;
  }
}
```

**Clamp hard.** ±0.2 % is ~3.5 cents — inaudible on program material and far more than the ~0.01 % you
actually need for crystal drift. If the error exceeds the clamp for a sustained period, something else is
wrong (a stall, a device change) and you should **resynchronise discontinuously** during a quiet passage
rather than pitch-bend your way back.

**Resampler choices:**
- **Browser (WASM):** [libsamplerate](https://github.com/libsndfile/libsamplerate) (`SRC_SINC_FASTEST`
  supports `src_set_ratio` for continuous variation) or Speex's resampler (small, in libopus's tree).
- **C++ side:** [soxr](https://sourceforge.net/projects/soxr/) is highest quality;
  `juce::LagrangeInterpolator` / `juce::CatmullRomInterpolator` are already in JUCE and are fine for
  ±0.2 % correction ratios (the interpolation error at ratios that close to 1.0 is negligible).

Whether you resample on the **sender** or **receiver** matters: correcting on the receiver keeps the
sender simple and lets you support multiple listeners with different clocks. **Correct on the receiver.**

### 4.4 Jitter buffer design

```
      network ──▶ [ reorder window ] ──▶ [ ring buffer ] ──▶ [ ASRC ] ──▶ AudioWorklet out
                    ~3 packets deep       target 20-60 ms      ±0.2%
```

- **Reorder window.** With `seq` in your header, hold a small window (3–5 packets ≈ 10–15 ms at 2.67 ms
  packets) and emit in order. UDP/DTLS reorders on Wi-Fi more than people expect.
- **Loss concealment.** Opus has built-in PLC: call `opus_decode(dec, NULL, 0, pcm, FRAME, 0)` for a lost
  packet. For **raw PCM there is no PLC** — you must implement something. Minimum viable: repeat the last
  period with a short cross-fade, or fade to silence over ~5 ms and back. A hard zero-fill click is
  unacceptable in a mix-referencing tool because the user will think it's a problem with *their mix*.
- **Adaptive target.** Track the observed inter-arrival jitter (RFC 3550 smoothed jitter estimate) and set
  `target = max(minTarget, k · jitter_p99)`. Grow the buffer **fast** (immediately, on an underrun) and
  shrink it **slowly** (only after N seconds of clean delivery), shrinking by resampling ratio rather
  than by dropping samples.
- **Expose the knob.** SonoBus's key UX insight: give the user manual *and* automatic jitter-buffer
  control so they can trade dropouts against latency themselves
  ([SonoBus user guide](https://sonobus.net/sonobus_userguide.html)). Do the same — a slider from
  "Lowest latency" to "Most stable" with the resulting ms shown.

### 4.5 How the prior art does it

**SonoBus** ([sonobus.net](https://sonobus.net/), GPLv3, JUCE-based — read the source, it is the single
most relevant codebase to this project):
- UDP with a forked **Audio-over-OSC** protocol; sequence-numbered packets reassembled into order in a
  per-peer receive jitter buffer.
- **Automatic resampling** between peers with different sample rates and drifting clocks — participants
  explicitly do not need matching settings.
- Per-user jitter buffer with auto and manual modes; the auto mode adapts to observed network timing.
- Opus at selectable bitrates *and* uncompressed PCM (16/24-bit) as options — same fidelity philosophy
  argued for in §2.1.

**Audiomovers LISTENTO** ([audiomovers.com/listento](https://audiomovers.com/listento)):
- User-selectable latency from **~50 ms up to 2 seconds** — an explicit, user-facing buffer control, again
  confirming that this is a UI decision, not just an algorithm.
- Codec ladder: PCM 16/24/32-bit lossless; AAC 96–320 kbps; Opus 128/256 kbps.
- **Adaptive bitrate** on the lossy modes, stepping down when the link degrades.
- Their own guidance notes AAC is more CPU-hungry and higher-latency than Opus.

The lesson from both: **the buffer size is a product decision surfaced to the user**, backed by an
automatic mode that is good enough for most people.

---

## 5. C++ implementation concerns

### 5.1 WebRTC library choice

| | **libdatachannel** | **libwebrtc** (Google) | **Pion** (Go) |
|---|---|---|---|
| Language | C++17, C API | C++ | Go |
| License | **MPL-2.0** (file-level copyleft; fine for a closed-source plugin if you link it unmodified) | BSD-3 | MIT |
| Build | **CMake, `git submodule update --init`, minutes** | `depot_tools` + GN + Ninja, hours, ~30 GB checkout, breaks constantly | `go build` |
| Binary size | **~20 MB Windows Release** (some report < 5 MB) | **~600 MB** (50 MB+ minimum) | ~10 MB |
| Dependencies | usrsctp, libjuice (or libnice), libsrtp, plog, nlohmann/json, OpenSSL/GnuTLS/MbedTLS | vendored everything | none (pure Go) |
| Codecs included | **none** — BYO encoder | full stack (Opus, VP8/9, AV1, AEC, NetEq…) | none |
| Media packetizers | H.264, H.265, AV1, **Opus**, G.711 (no VP8/VP9) | everything | everything |
| WebSocket server | **yes, built in** | no | via other pion libs |
| Platforms | Linux, macOS, iOS, Windows, Android, FreeBSD | all | all |

Sources: [libdatachannel](https://github.com/paullouisageneau/libdatachannel),
[TensorWorks: a brief comparison of libdatachannel and libWebRTC](https://tensorworks.com.au/blog/a-brief-comparison-of-libdatachannel-and-libwebrtc/),
[pion/webrtc](https://github.com/pion/webrtc).

**Recommendation: libdatachannel.** It is the only sane choice for an audio plugin.

- Building libwebrtc inside a plugin project is a career-limiting move: multi-GB checkout, `depot_tools`,
  GN, a 600 MB static library, and constant ABI churn. You would also inherit its AEC/AGC/NetEq stack,
  which you actively do not want for music.
- libdatachannel gives you exactly the pieces you need — ICE, DTLS, SCTP data channels, SRTP media — and
  nothing else. You bring `libopus` yourself, which you were going to do anyway.
- It **also ships a WebSocket client and server**, so you can use one library for signalling *and*
  media. That is a real simplification.
- **Pion** is excellent but it's Go. Embedding a Go runtime in a VST3 that the host loads into its process
  is asking for trouble (signal handling, TLS/goroutine scheduler interactions, thread-local assumptions).
  Note also that the widely-repeated claim "Pion needs cgo/OpenSSL for DTLS" is **outdated** — `pion/dtls`
  has been pure Go for years. It's still the wrong shape for an in-process plugin.

**Opus over an SRTP track with libdatachannel:**

```cpp
#include <rtc/rtc.hpp>

auto pc = std::make_shared<rtc::PeerConnection>(config);

rtc::Description::Audio media("audio", rtc::Description::Direction::SendOnly);
media.addOpusCodec(96);
media.addSSRC(ssrc, "audio-send");
auto track = pc->addTrack(media);

auto rtpConfig  = std::make_shared<rtc::RtpPacketizationConfig>(
    ssrc, "audio-send", 96, rtc::OpusRtpPacketizer::DefaultClockRate /* 48000 */);
auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
track->setMediaHandler(packetizer);

// Per encoded Opus frame (10 ms @ 48 kHz = 480 samples):
track->send(reinterpret_cast<const std::byte*>(opusBytes), opusLen);
rtpConfig->timestamp += 480;   // advance by the frame duration in samples
```

(See [`examples/streamer/main.cpp`](https://github.com/paullouisageneau/libdatachannel/blob/master/examples/streamer/main.cpp)
and [discussion #1244](https://github.com/paullouisageneau/libdatachannel/discussions/1244).)

**Unreliable data channel for PCM:**

```cpp
rtc::DataChannelInit init;
init.reliability.unordered   = true;
init.reliability.maxRetransmits = 0;     // unreliable
auto dc = pc->createDataChannel("audio", init);
dc->send(reinterpret_cast<const std::byte*>(packet), packetSize);
```

### 5.2 Embedding an HTTP server in C++

| | **cpp-httplib** | **uWebSockets** | **civetweb** |
|---|---|---|---|
| Language | C++11, **single header** | C++17/20 | C, C++ wrapper |
| License | MIT | Apache-2.0 | MIT |
| Concurrency | blocking I/O + thread pool | event loop (µSockets/libuv) | thread-per-connection pool |
| TLS | OpenSSL or MbedTLS | OpenSSL / BoringSSL (via µSockets) | OpenSSL |
| WebSocket | yes | **yes — it's the whole point** | yes |
| Scale | "not built for massive numbers of simultaneous connections" | very high | moderate |
| Integration pain | **lowest** | moderate (µSockets submodule) | low |

Sources: [cpp-httplib](https://yhirose.github.io/cpp-httplib/en/),
[civetweb](https://github.com/civetweb/civetweb) + [OpenSSL notes](http://civetweb.github.io/civetweb/OpenSSL.html),
[uWebSockets](https://github.com/uNetworking/uWebSockets).

**Recommendation:**
- If you use **libdatachannel**, use its **built-in `rtc::WebSocketServer`** for signalling and skip the
  extra dependency entirely. You still need something to serve the static page — cpp-httplib is 1 header.
- If you go the **WebSocket-transport** route (§1.7), use **uWebSockets** — it's built for exactly this and
  its backpressure handling (`ws->getBufferedAmount()`) matters when the phone's Wi-Fi degrades.
- **cpp-httplib** for serving the SPA + QR + config JSON: drop in one header, done.

```cpp
#include "httplib.h"

httplib::SSLServer svr("cert.pem", "key.pem");   // needs CPPHTTPLIB_OPENSSL_SUPPORT
svr.set_mount_point("/", "./webui");
svr.Get("/api/session", [&](const httplib::Request& req, httplib::Response& res) {
    res.set_content(sessionJson(), "application/json");
});
svr.listen("0.0.0.0", 8443);
```

**Threading discipline (plugin-specific, and easy to get wrong):** none of this may touch the audio
thread. `processBlock()` must only push into a lock-free SPSC ring
(`juce::AbstractFifo` / `moodycamel::ReaderWriterQueue`); a dedicated network thread drains it, encodes,
and sends. No allocation, no locks, no logging, no `send()` on the audio thread — ever.

### 5.3 ⚠️ TLS and secure contexts — the critical constraint

**Question: does `http://<lan-ip>` count as a secure context? Answer: NO.**

The [W3C Secure Contexts spec §3.1](https://www.w3.org/TR/secure-contexts/) defines "potentially
trustworthy origin" as exactly:

1. scheme is `https` or `wss`;
2. host matches the CIDR `127.0.0.0/8` **or** `::1/128`;
3. host is `localhost` or ends in `.localhost`;
4. scheme is `file`;
5. a UA-specific scheme (`chrome-extension:`, `app:`);
6. an origin the developer manually configured as trustworthy.

**`192.168.0.0/16`, `10.0.0.0/8`, and `172.16.0.0/12` are conspicuously absent.** RFC 1918 space is *not*
loopback and is *not* trusted. The phone is a different device from the server, so nothing about the
loopback exemption applies.

**What is therefore blocked on `http://192.168.1.50:8080`:**

| API | Blocked on plain-http LAN IP? | Consequence for this project |
|---|---|---|
| `AudioWorklet` (`BaseAudioContext.audioWorklet`) | **YES** | 💀 No sample-accurate playback path at all |
| WebCodecs (`AudioDecoder`) | **YES** | 💀 No native Opus decode |
| `WebTransport` | **YES** | 💀 |
| `SharedArrayBuffer` (also needs COOP/COEP) | **YES** | 💀 No lock-free ring to the worklet |
| Service Worker / Cache API | **YES** | No offline PWA |
| `getUserMedia` | **YES** | (only matters for talkback) |
| `navigator.wakeLock` | **YES** | 💀 Can't keep the screen awake — and iOS suspends audio on lock (§1.2) |
| `crypto.subtle` | **YES** | No WebCrypto for pairing |
| `RTCPeerConnection` | **No** (not secure-context-gated in Chrome/Safari today) | WebRTC still constructs… |
| `<audio>` / `AudioContext` basics / `WebSocket ws://` | No | …the crude path still works |

Ref: [MDN AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet) —
*"Secure context: This feature is available only in secure contexts (HTTPS)."*
[MDN Secure Contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts).

**Conclusion: you must serve the page over HTTPS. There is no way around it.** Everything interesting is
behind that gate.

#### Options for HTTPS on a LAN IP, ranked

**1. ⭐ Public DNS name that resolves to the private IP + a real Let's Encrypt certificate (the Plex pattern).**

You run (or use) a DNS zone where `A-B-C-D.yourdomain.dev` resolves to `A.B.C.D`, and you obtain a
**wildcard** cert for `*.yourdomain.dev` via a **DNS-01** challenge (DNS-01 works fine for names that
resolve to private IPs, because the CA validates a TXT record, never connects to the host). Ship the cert
+ key with the plugin, or fetch per-install certs from your backend.

- Plex has done this at scale for a decade. [tlsmy.net](https://github.com/supersat/tlsmy.net) is a public
  implementation of exactly this pattern; so is
  [local-ip.medicmobile.org](https://github.com/medic/nginx-local-ip) and
  [lancert](https://lucor.dev/blog/lancert-lets-encrypt-certificates-local-development/).
- **Do not just use `nip.io` / `sslip.io`** — the free services resolve names but
  [do not provide wildcard certificates](https://nip.io/), and rate limits will bite you.
- **Trade-off:** you're shipping a private key inside a desktop app; assume it will be extracted. Mitigate
  with per-install certs (short-lived, issued by your backend on activation) rather than one global key.
  Also: it requires **internet access to resolve the DNS name**, which breaks the offline-studio case
  unless you also write a `hosts`-style fallback or ship the resolution client-side. Mitigate by caching:
  public DNS TTLs are respected, so a phone that has resolved once usually keeps working.

**2. Self-signed cert + one-time trust.**

Generate a cert on first run, serve it, and walk the user through accepting the warning.

- iOS: the interstitial has "Show Details → visit this website" — it works, but for a **WebSocket over
  TLS** the exception must be established via a top-level navigation first, and iOS is inconsistent about
  persisting it. To do it properly the user must install a **configuration profile** and then enable it
  under Settings → General → About → **Certificate Trust Settings** — a five-step flow you will lose
  users in.
- Android Chrome: "Advanced → Proceed" works, is per-origin, and is reasonably sticky.
- **Verdict:** acceptable as a fallback, terrible as the primary flow.

**3. `WebTransport` `serverCertificateHashes`.**

Pins a self-signed cert by hash (§1.9). Solves the *transport* trust but not the *page origin*, so it
doesn't get you `AudioWorklet`. Only useful in combination with option 1 or as a native-app transport.

**4. Chrome flag `--unsafely-treat-insecure-origin-as-secure`.**

Development only. Cannot be set on a phone browser. Not a product option.

**5. Run the whole UI in a native companion app (WKWebView/WebView with a custom URL scheme handler).**

Sidesteps the entire problem — a custom scheme can be registered as trustworthy. But then you're shipping
an app to two stores, which is exactly the friction the browser approach exists to avoid.

#### The other browser-side gate: Local Network Access

Separately from secure contexts, Chrome 141 (2025-09-30) began rolling out and Chrome **142 launched** the
**Local Network Access permission prompt**: a request *from* the public network *to* a local/loopback
destination now requires user permission.
Refs: [Chrome for Developers: New permission prompt for Local Network Access](https://developer.chrome.com/blog/local-network-access),
[WICG explainer](https://github.com/WICG/local-network-access/blob/main/explainer.md).

Crucially for this project:
- **Local → local is not currently gated.** Once the phone has loaded a page served *from* the LAN IP,
  that page's requests back to the LAN are same-address-space and unaffected. *(Chrome says it plans to
  extend this to "all cross-origin requests going to destinations on the local network" — watch it.)*
- **Top-level navigation is not gated.** Scanning a QR that navigates directly to the LAN server is fine.
  The explainer notes top-level navigation "remains a risk" and floats a future interstitial, but that is
  not shipped.
- **WebSockets, WebTransport, and WebRTC are listed as known gaps** — not yet covered by the prompt, but
  explicitly on the roadmap ("Local connection attempts that use WebRTC should also be gated").
- **The permission is secure-context-restricted**, so a hybrid design where a cloud-hosted `https://` page
  reaches into the LAN will get a prompt (and will also hit mixed content unless it uses
  `targetAddressSpace: 'local'`). Safari has its own, stricter Local Network permission on iOS 18+.

**Architectural implication:** serve the page **from** the desktop app on the LAN. Do *not* build the
"cloud page reaches into your LAN" design — it is being actively deprecated across browsers.

### 5.4 Recommended architecture

```
┌──────────────────── Desktop (VST3/AU plugin + helper) ────────────────────┐
│                                                                            │
│  processBlock()  ──lock-free SPSC ring──▶  Network thread                  │
│  (audio thread,                            ├─ PCM24 packetizer            │
│   no alloc/locks)                          ├─ libopus encoder (10 ms)     │
│                                            └─ libFLAC encoder (256-blk)   │
│                                                       │                    │
│  cpp-httplib SSLServer :8443 ── serves SPA + QR + /api/session             │
│  libdatachannel ── WebSocketServer (signalling) + PeerConnection (media)   │
│  cert: *.lan.yourdomain.dev, DNS-01, per-install, auto-renewed             │
└────────────────────────────────────────────────────────────────────────────┘
                     │  QR: https://192-168-1-50.lan.yourdomain.dev:8443/#t=…
                     ▼
┌──────────────────────── Phone browser (secure context ✅) ────────────────┐
│  SPA ─ WebRTC unreliable DataChannel  (or wss:// fallback)                │
│      ─ WebCodecs AudioDecoder (Opus)  │ WASM libFLAC │ raw PCM            │
│      ─ ring buffer (SharedArrayBuffer or postMessage transfer)            │
│      ─ AudioWorklet + ASRC (PI-controlled, ±0.2 % clamp)                  │
│      ─ wakeLock, level meters, latency readout, buffer slider             │
└───────────────────────────────────────────────────────────────────────────┘
```

**Phased plan:**

1. **Phase 0 — solve HTTPS first.** Nothing else can be validated until the phone loads a secure-context
   page from the desktop. Prototype the DNS-name + DNS-01 wildcard flow before writing any audio code.
   This is the project's highest-risk item and it is entirely non-obvious.
2. **Phase 1** — cpp-httplib + WebSocket (uWebSockets or libdatachannel's), **24-bit PCM, 128-frame
   packets**, AudioWorklet ring, fixed 40 ms buffer. Proves fidelity and measures real latency.
3. **Phase 2** — ASRC drift correction + adaptive jitter buffer + PLC.
4. **Phase 3** — swap transport to libdatachannel unreliable data channel; add Opus for the WAN path with
   WebCodecs decode and a WASM fallback.
5. **Phase 4** — internet path: STUN, a hosted TURN (coturn), account/relay signalling.

---

## 6. Open questions to resolve empirically

1. **What is `AudioContext.outputLatency` on real target phones?** If a mid-range Android reports 120 ms,
   your entire transport optimisation is noise. Measure this before committing to a latency target.
2. **Does iOS Safari's audio suspend-on-lock have any mitigation in 2026?** Test whether a
   `MediaSession`-backed `<audio>` element kept alive alongside the `AudioContext` changes the behaviour.
   If not, the wake-lock + "keep screen on" warning is the whole answer.
3. **Does Chrome's LNA roadmap gate local→local yet?** Retest on the current Chrome each release.
4. **Measure Opus 510 kbps vs 24-bit PCM in a blind test with your actual users** on their actual phone
   playback chains. If PCM's advantage is inaudible through an iPhone speaker or cheap IEMs, the WAN path
   gets much easier — but the *option* of lossless is a credibility feature for this audience regardless.
5. **VST3 sandbox / network permissions:** confirm that hosts (Logic's AU sandbox especially, and
   notarised/hardened-runtime macOS builds) permit listening sockets from inside the plugin. If not, you
   need an out-of-process helper — which changes the whole deployment story.

---

## 7. Sources

**Secure contexts / browser platform**
- https://www.w3.org/TR/secure-contexts/
- https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts
- https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet
- https://developer.chrome.com/blog/local-network-access
- https://github.com/WICG/local-network-access/blob/main/explainer.md
- https://developer.chrome.com/blog/private-network-access-update

**Transports**
- https://webkit.org/blog/17862/webkit-features-for-safari-26-4/
- https://webrtc.ventures/2026/04/webtransport-is-now-baseline-what-it-means-for-real-time-media/
- https://github.com/w3c/webtransport/blob/main/explainer.md
- https://developer.mozilla.org/docs/Web/API/WebTransport/WebTransport
- https://groups.google.com/a/chromium.org/g/blink-dev/c/m0v9XiwKA4M/m/GtMq9j_iAAAJ
- https://developer.mozilla.org/en-US/docs/Web/API/RTCRtpReceiver/jitterBufferTarget
- https://groups.google.com/a/chromium.org/g/blink-dev/c/4W4orKqA3Rs
- https://bugzilla.mozilla.org/show_bug.cgi?id=1631263
- https://issues.webrtc.org/issues/41481053
- https://www.mux.com/articles/rtmp-vs-srt-vs-webrtc-live-streaming-ingest-protocols
- https://www.wink.co/documentation/Ultra-Low-Latency-HLS-Experiments-2025
- https://www.forasoft.com/blog/article/webrtc-video-steaming-app-vs-hls
- https://hpbn.co/wifi/

**Codecs**
- https://opus-codec.org/docs/opus_api-1.5/group__opus__encoderctls.html
- https://arxiv.org/pdf/1602.04845 (Valin et al., low-delay music coding in Opus)
- https://wiki.hydrogenaudio.org/index.php?title=Opus
- https://www.rfc-editor.org/info/rfc9639/ (FLAC)
- https://xiph.org/flac/features.html
- https://en.wikipedia.org/wiki/AAC-LD
- https://www.comrex.com/resources/algorithm-chart/
- https://developer.mozilla.org/en-US/docs/Web/API/WebCodecs_API/Codec_selection
- https://webcodecsfundamentals.org/audio/encoded-audio-chunk/

**Discovery**
- https://groups.google.com/a/chromium.org/g/chromium-discuss/c/6b0vVreNTvQ
- https://developer.chrome.com/docs/apps/reference/mdns
- https://datatracker.ietf.org/doc/html/draft-ietf-mmusic-mdns-ice-candidates-03
- https://bloggeek.me/psa-mdns-and-local-ice-candidates-are-coming/
- https://github.com/mjansson/mdns
- https://github.com/nayuki/QR-Code-generator

**Clock drift / prior art**
- https://sonobus.net/sonobus_userguide.html
- https://github.com/sonosaurus/sonobus
- https://audiomovers.com/listento
- https://ftp.audiomovers.com/what-audio-quality-settings-are-right-for-you/
- https://padenot.github.io/web-audio-perf/
- https://0110.be/posts/Resampling_audio_via_a_Web_Audio_API_Audio_Worklet
- https://github.com/godotengine/godot/issues/36643
- https://bugs.webkit.org/show_bug.cgi?id=154538
- https://developer.apple.com/forums/thread/774239

**C++ libraries**
- https://github.com/paullouisageneau/libdatachannel
- https://libdatachannel.org/
- https://tensorworks.com.au/blog/a-brief-comparison-of-libdatachannel-and-libwebrtc/
- https://github.com/paullouisageneau/libdatachannel/blob/master/examples/streamer/main.cpp
- https://github.com/pion/webrtc
- https://yhirose.github.io/cpp-httplib/en/
- https://github.com/civetweb/civetweb
- http://civetweb.github.io/civetweb/OpenSSL.html
- https://github.com/uNetworking/uWebSockets
- https://github.com/Haivision/srt

**TLS on LAN**
- https://github.com/supersat/tlsmy.net
- https://lucor.dev/blog/lancert-lets-encrypt-certificates-local-development/
- https://github.com/medic/nginx-local-ip
- https://nip.io/
- https://github.com/pyrou/traefik.me
