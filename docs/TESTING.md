# Manual acceptance checklist

The automated suite covers the protocol, the buffers and the arithmetic. It cannot cover
the parts that decide whether this is usable: whether a DAW loads the plugin, whether the
operating system lets it listen, and whether a person holding a phone hears their mix.

This is the list for those. It is written as pass/fail — nothing says "should" — and the
boxes are **unticked on purpose**. They record what has and has not been verified, and
they are copied from the risk review in [`PLAN-risks.md`](PLAN-risks.md) §4. If you run
any of these, a pull request ticking the box with what you saw is a real contribution.

## What has been verified

These were run during development on macOS 26 / Apple silicon, and their results are the
basis for the claims in the README:

- [x] `git clone && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
      succeeds with no manual JUCE checkout: 26 s configure, 56 s build.
- [x] `ctest` green — 45 C++ tests plus 12 receiver tests.
- [x] `auval -v aufx Ppm1 Ppmx` prints **PASS** and does not hang.
- [x] `pluginval --strictness-level 5` exits 0 for both the VST3 and the AudioUnit.
- [x] Build and full test suite green in CI on `macos-14`, `windows-2022` and
      `ubuntu-22.04`.
- [x] The standalone build serves the page and streams to a real client over the LAN:
      428 packets in 5.0 s, 0 lost, 1.4 Mbit/s at 44.1 kHz stereo pcm16, and the captured
      WAV is 4.97 s long — so packets are paced in real time, not bursted.
- [x] A WebSocket upgrade without the pairing token is refused with `403` before any audio
      byte is sent.
- [x] **No macOS Local Network permission prompt appeared** while the standalone was
      listening. This is the property that the whole network design protects; if a prompt
      ever appears, something has added an outbound connection, a `.local` lookup or a
      Bonjour call.
- [x] 60 server start/stop cycles leave the process's open file-descriptor count flat.

## Hosts — not yet verified

Each of these is one person, one afternoon, and would materially change what the README
can claim.

- [ ] Loads and streams in **Ableton Live**.
- [ ] Loads and streams in **Logic Pro**. This is the AudioUnit sandbox test and the one
      most likely to fail: Logic runs AUs in a sandboxed process with a different set of
      rules from a normal host.
- [ ] Loads and streams in **GarageBand**.
- [ ] Loads and streams in **Reaper** and in **JUCE AudioPluginHost**.
- [ ] Loads and streams in a DAW on **Windows**, and the Defender Firewall prompt (which
      will name the DAW, not PhonePostMix) is survivable.
- [ ] Loads and streams in a DAW on **Linux**.
- [ ] **No Local Network permission prompt** in any macOS host.
- [ ] Audio passes through **bit-identically** with streaming on and off — verified with a
      null test, not by ear.
- [ ] Transport stopped: the phone stays connected and shows that the host is not playing,
      rather than disconnecting. (The silence keepalive exists for this; it has been tested
      against a synthetic client, not against a real host that stops calling
      `processBlock`.)
- [ ] Host bypass engaged: the connection survives.
- [ ] Offline bounce: the stream pauses, the bounce runs at full speed, no buffer overruns.
- [ ] Project sample rate changed mid-session: the phone resyncs at the new rate within
      2 s, with no pitch shift.
- [ ] Interface buffer size changed mid-stream: no dropout on the DAW side.
- [ ] Two instances loaded: the second binds the next port and the first is unaffected.
- [ ] Closing the project with streaming active returns in under a second, with no hang.

## Receiver — not yet verified on hardware

**Nothing in this section has been run on a physical phone.** The receiver's logic is
tested under Node; its behaviour in a real mobile browser is not.

- [ ] iPhone on current iOS and a mid-range Android phone, both on the same Wi-Fi.
- [ ] QR scan → page loads → tap **LISTEN** → audio, in under 15 seconds, no typing.
- [ ] The diagnostics panel shows secure context, context sample rate, buffer fill in ms,
      packets lost and connection state.
- [ ] iPhone with the **silent switch on**: either audio plays (because
      `navigator.audioSession.type = 'playback'` worked) or the page's hint is visible.
- [ ] **Screen lock behaviour matches what the README says.** The expectation is that iOS
      suspends the audio and the page cannot prevent it. Confirm it, and confirm the page
      recovers when you come back.
- [ ] Walking out of Wi-Fi range and back: audio recovers within 5 s with no page reload,
      and latency does **not** stay permanently higher afterwards.
- [ ] **20-minute continuous soak** with no audible dropouts, and buffer fill within
      ±30 ms of target at the end. This is the "works for a session, not for two minutes"
      test, and it is the one the drift controller exists to pass.
- [ ] Two phones connected at once: both play, neither disturbs the other.
- [ ] `ScriptProcessorNode` buffer size: confirm 4096 is right, and find the smallest size
      that does not glitch on a mid-range Android while scrolling the page.

## Numbers still to measure

The README's latency table is computed from the code. These would replace it with
measurements:

- [ ] End-to-end latency with wired earbuds, measured with a microphone and a click.
- [ ] The same measurement on AirPods, to put a real number next to "use wired
      headphones".
- [ ] Observed clock drift in ppm over a 20-minute session, and how long the controller
      takes to settle.

## Robustness

- [ ] 10 kB of random bytes thrown at the HTTP port does not crash or hang the DAW.
- [ ] A client that connects and then stops reading entirely does not grow the plugin's
      memory without bound. (`serverDropsOldestFramesWhenAClientStopsReading` covers the
      queue; this is about the whole process over a long session.)
