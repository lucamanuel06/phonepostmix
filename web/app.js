/*  PhonePostMix receiver.

    One file, no build step, no modules, no external requests. It is served by the plugin
    itself, so every byte here travels over the same LAN connection as the audio.

    The shape of this file is dictated by one fact: a page served from
    http://192.168.x.x is NOT a secure context. RFC 1918 addresses are not on the W3C
    "potentially trustworthy" list — only 127.0.0.0/8, ::1 and localhost are. That removes
    AudioWorklet, SharedArrayBuffer, WebCodecs, service workers and navigator.wakeLock,
    permanently, on every browser. So:

      - playback runs on ScriptProcessorNode, which is deprecated but not gated;
      - the ring buffer is a plain Float32Array, because with ScriptProcessorNode both the
        socket callback and the audio callback run on the main thread and there is no
        cross-thread hazard to protect against;
      - the page tells the user about screen lock instead of preventing it.

    There is deliberately no AudioWorklet path. A branch that cannot execute on any phone
    is not a fallback, it is dead code that rots and lies to you in review. Capability
    detection still reports whether AudioWorklet exists, because that number is what will
    justify building the HTTPS path later. See docs/adr/0003.
*/
(function () {
  'use strict';

  // ---------------------------------------------------------------------------
  // Protocol constants. These mirror src/core/WirePacket.h and docs/protocol.md.
  // ---------------------------------------------------------------------------
  var MAGIC = 0x584d5050;      // 'PPMX' read as a little-endian uint32
  var VERSION = 1;
  var HEADER_BYTES = 32;
  var FORMAT_PCM16 = 0, FORMAT_PCM24 = 1, FORMAT_FLOAT32 = 2;
  var FLAG_DISCONTINUITY = 1, FLAG_SILENCE = 2, FLAG_CONFIG_CHANGED = 4;

  var RING_FRAMES = 1 << 18;   // ~5.5 s at 48 kHz; 2 MB, and overflow becomes impossible
  var SPN_BUFFER = 4096;       // 85 ms at 48 kHz — the mobile-safe ScriptProcessorNode size

  // ---------------------------------------------------------------------------
  // Capability detection. Drives both the audio path and the diagnostics panel.
  // ---------------------------------------------------------------------------
  var AudioContextClass = window.AudioContext || window.webkitAudioContext;

  var caps = {
    secureContext: window.isSecureContext === true,
    crossOriginIsolated: self.crossOriginIsolated === true,
    audioContext: !!AudioContextClass,
    audioWorklet: !!(AudioContextClass && AudioContextClass.prototype &&
                     'audioWorklet' in AudioContextClass.prototype),
    sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
    audioDecoder: 'AudioDecoder' in window,
    webTransport: 'WebTransport' in window,
    wakeLock: 'wakeLock' in navigator,
    audioSession: 'audioSession' in navigator
  };

  // ---------------------------------------------------------------------------
  // DOM
  // ---------------------------------------------------------------------------
  var el = {};
  ['listen', 'status', 'statusText', 'dot', 'subtitle', 'meterL', 'meterR', 'volume',
   'volumeValue', 'buffer', 'bufferValue', 'noteWakeLock', 'noteBluetooth', 'noteSilent',
   'diagnostics', 'copy', 'keepAlive'].forEach(function (id) { el[id] = document.getElementById(id); });

  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------
  var ctx = null;
  var node = null;
  var gain = null;
  var audioPath = 'none';
  var playing = false;
  var wakeLock = null;

  var ring = { l: new Float32Array(RING_FRAMES), r: new Float32Array(RING_FRAMES), write: 0, read: 0 };

  var stream = {
    sampleRate: 48000, channels: 2, format: FORMAT_PCM16, framesPerPacket: 512,
    configEpoch: -1, hostPlaying: false
  };

  var stats = {
    packets: 0, bytes: 0, lost: 0, reordered: 0, underruns: 0, resyncs: 0,
    lastSeq: -1, connectedAt: 0, reconnects: 0, bytesWindow: 0, kbitPerSecond: 0
  };

  var target = { ms: 120 };
  var drift = { integral: 0, correction: 0, ppm: 0, outOfRangeSince: 0 };
  var resampler = { phase: 0, prevL: 0, prevR: 0, primed: false };
  var meter = { l: 0, r: 0 };
  var conceal = { active: false, gain: 1 };

  var socket = null;
  var reconnectDelay = 500;
  var reconnectTimer = null;
  var token = (location.hash.match(/[#&]t=([0-9a-fA-F]+)/) || [])[1] || '';

  // ---------------------------------------------------------------------------
  // Ring buffer helpers. Indices are plain numbers modulo RING_FRAMES.
  // ---------------------------------------------------------------------------
  function ringFill() {
    return (ring.write - ring.read + RING_FRAMES) % RING_FRAMES;
  }

  function ringReset() {
    ring.read = 0;
    ring.write = 0;
    resampler.primed = false;
    resampler.phase = 0;
    drift.integral = 0;
    drift.correction = 0;
  }

  function ringPush(left, right, count) {
    // Overflow policy: if the page has fallen far behind — a backgrounded tab, a stalled
    // audio callback — discard down to the target instead of letting latency grow without
    // bound. TCP will happily buffer minutes of audio for us; nobody wants to hear it.
    var limit = Math.min(RING_FRAMES - 2, Math.floor(targetFrames() * 3));
    if (ringFill() + count > limit) {
      ring.read = (ring.write + RING_FRAMES - Math.floor(targetFrames())) % RING_FRAMES;
      stats.resyncs++;
    }

    var w = ring.write;
    for (var i = 0; i < count; i++) {
      ring.l[w] = left[i];
      ring.r[w] = right[i];
      w = (w + 1) % RING_FRAMES;
    }
    ring.write = w;
  }

  function targetFrames() {
    return target.ms * 0.001 * stream.sampleRate;
  }

  // ---------------------------------------------------------------------------
  // Packet parsing
  // ---------------------------------------------------------------------------
  var scratchL = new Float32Array(8192);
  var scratchR = new Float32Array(8192);

  function handlePacket(buffer) {
    if (buffer.byteLength < HEADER_BYTES) return;

    var view = new DataView(buffer);
    if (view.getUint32(0, true) !== MAGIC) return;
    if (view.getUint8(4) !== VERSION) { setStatus('error', 'unsupported protocol'); return; }

    var format = view.getUint8(5);
    var channels = view.getUint8(6);
    var flags = view.getUint8(7);
    var seq = view.getUint32(8, true);
    var sampleRate = view.getUint32(12, true);
    var frames = view.getUint16(16, true);
    var epoch = view.getUint16(18, true);

    if (epoch !== stream.configEpoch || sampleRate !== stream.sampleRate ||
        channels !== stream.channels || format !== stream.format) {
      stream.configEpoch = epoch;
      stream.sampleRate = sampleRate;
      stream.channels = channels;
      stream.format = format;
      stream.framesPerPacket = frames;
      ringReset();
      renderSubtitle();
    }

    if (flags & FLAG_DISCONTINUITY) { ringReset(); }

    // Sequence accounting. Over TCP a gap means the sender dropped packets under
    // backpressure, not that the network lost them — either way the receiver has a hole.
    if (stats.lastSeq >= 0) {
      var delta = (seq - stats.lastSeq) >>> 0;
      if (delta === 0 || delta > 0x80000000) stats.reordered++;
      else if (delta > 1) stats.lost += delta - 1;
    }
    stats.lastSeq = seq;
    stats.packets++;
    stats.bytes += buffer.byteLength;
    stats.bytesWindow += buffer.byteLength;

    if (frames > scratchL.length) {
      scratchL = new Float32Array(frames);
      scratchR = new Float32Array(frames);
    }

    decode(view, buffer, format, channels, frames);
    ringPush(scratchL, scratchR, frames);
  }

  function decode(view, buffer, format, channels, frames) {
    var i, offset = HEADER_BYTES;

    if (format === FORMAT_FLOAT32) {
      // HEADER_BYTES is a multiple of 4 precisely so this view is legal without a copy.
      var floats = new Float32Array(buffer, offset, frames * channels);
      for (i = 0; i < frames; i++) {
        scratchL[i] = floats[i * channels];
        scratchR[i] = channels > 1 ? floats[i * channels + 1] : floats[i * channels];
      }
      return;
    }

    if (format === FORMAT_PCM16) {
      for (i = 0; i < frames; i++) {
        var base16 = offset + i * channels * 2;
        scratchL[i] = view.getInt16(base16, true) / 32767;
        scratchR[i] = channels > 1 ? view.getInt16(base16 + 2, true) / 32767 : scratchL[i];
      }
      return;
    }

    if (format === FORMAT_PCM24) {
      for (i = 0; i < frames; i++) {
        var base24 = offset + i * channels * 3;
        scratchL[i] = int24(view, base24) / 8388607;
        scratchR[i] = channels > 1 ? int24(view, base24 + 3) / 8388607 : scratchL[i];
      }
    }
  }

  function int24(view, offset) {
    var value = view.getUint8(offset) | (view.getUint8(offset + 1) << 8) | (view.getUint8(offset + 2) << 16);
    return value & 0x800000 ? value - 0x1000000 : value;
  }

  // ---------------------------------------------------------------------------
  // Playback. One pull() feeds whichever node we ended up with.
  // ---------------------------------------------------------------------------
  function updateDrift(blockFrames) {
    var fill = ringFill();
    var wanted = targetFrames();
    if (wanted <= 0) return;

    var err = (fill - wanted) / wanted;
    drift.integral += err * (blockFrames / ctx.sampleRate);
    drift.integral = Math.max(-50, Math.min(50, drift.integral));   // anti-windup

    var correction = 2e-5 * err + 1e-6 * drift.integral;
    var clamped = Math.max(-0.002, Math.min(0.002, correction));    // +/-0.2%, ~3.5 cents

    // Staying pinned at the clamp means something structural is wrong — a stall, a device
    // change, a sample rate we missed. Resynchronise abruptly rather than pitch-bending
    // back over minutes.
    if (clamped !== correction) {
      if (!drift.outOfRangeSince) drift.outOfRangeSince = performance.now();
      else if (performance.now() - drift.outOfRangeSince > 5000) {
        ring.read = (ring.write + RING_FRAMES - Math.floor(wanted)) % RING_FRAMES;
        drift.integral = 0;
        drift.outOfRangeSince = 0;
        stats.resyncs++;
      }
    } else {
      drift.outOfRangeSince = 0;
    }

    drift.correction = clamped;
    drift.ppm = clamped * 1e6;
  }

  function pull(outL, outR, count) {
    if (!playing) { outL.fill(0); outR.fill(0); return; }

    updateDrift(count);

    var ratio = (stream.sampleRate / ctx.sampleRate) * (1 + drift.correction);
    var peakL = 0, peakR = 0;

    for (var i = 0; i < count; i++) {
      // Wait until the buffer has filled to target before starting, so the first thing
      // the listener hears is not an immediate underrun.
      if (!resampler.primed) {
        if (ringFill() < targetFrames()) { outL[i] = 0; outR[i] = 0; continue; }
        resampler.primed = true;
        resampler.phase = 0;
      }

      while (resampler.phase >= 1) {
        if (ringFill() < 2) {
          // Underrun. Fade what we have rather than emitting a hard zero: in a mix
          // referencing tool a click reads as a problem with the mix, not the network.
          if (!conceal.active) { conceal.active = true; conceal.gain = 1; stats.underruns++; }
          break;
        }
        resampler.prevL = ring.l[ring.read];
        resampler.prevR = ring.r[ring.read];
        ring.read = (ring.read + 1) % RING_FRAMES;
        resampler.phase -= 1;
        conceal.active = false;
        conceal.gain = 1;
      }

      var nextIndex = ring.read;
      var nextL = ring.l[nextIndex], nextR = ring.r[nextIndex];
      var t = resampler.phase;
      var sl = resampler.prevL + (nextL - resampler.prevL) * t;
      var sr = resampler.prevR + (nextR - resampler.prevR) * t;

      if (conceal.active) {
        conceal.gain *= 0.9995;            // ~20 ms fade to silence at 48 kHz
        sl = resampler.prevL * conceal.gain;
        sr = resampler.prevR * conceal.gain;
      }

      outL[i] = sl;
      outR[i] = sr;
      resampler.phase += ratio;

      var al = sl < 0 ? -sl : sl, ar = sr < 0 ? -sr : sr;
      if (al > peakL) peakL = al;
      if (ar > peakR) peakR = ar;
    }

    meter.l = Math.max(peakL, meter.l * 0.82);
    meter.r = Math.max(peakR, meter.r * 0.82);
  }

  function createNode() {
    var spn = ctx.createScriptProcessor(SPN_BUFFER, 0, 2);
    spn.onaudioprocess = function (event) {
      pull(event.outputBuffer.getChannelData(0), event.outputBuffer.getChannelData(1), SPN_BUFFER);
    };
    audioPath = 'spn';
    return spn;
  }

  // ---------------------------------------------------------------------------
  // Transport
  // ---------------------------------------------------------------------------
  function connect() {
    if (socket) return;

    setStatus('connecting', 'connecting');

    var url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host +
              '/ws' + (token ? '?t=' + encodeURIComponent(token) : '');

    socket = new WebSocket(url);
    socket.binaryType = 'arraybuffer';

    socket.onopen = function () {
      reconnectDelay = 500;
      stats.connectedAt = performance.now();
      setStatus('live', 'connected');
      send({
        type: 'ready', protocol: VERSION, ctxSampleRate: ctx ? ctx.sampleRate : null,
        path: audioPath, caps: caps, targetLatencyMs: target.ms, ua: navigator.userAgent
      });
    };

    socket.onmessage = function (event) {
      if (typeof event.data === 'string') handleControl(event.data);
      else handlePacket(event.data);
    };

    socket.onclose = function () {
      socket = null;
      setStatus('error', 'disconnected');
      ringReset();
      scheduleReconnect();
    };

    socket.onerror = function () { setStatus('error', 'connection failed'); };
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(function () {
      reconnectTimer = null;
      stats.reconnects++;
      connect();
    }, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 5000);
  }

  function send(object) {
    if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(object));
  }

  function handleControl(text) {
    var message;
    try { message = JSON.parse(text); } catch (e) { return; }

    // Unknown types and unknown keys are ignored, deliberately: that is the entire
    // forward-compatibility contract, and honouring it costs nothing.
    if (message.type === 'hello' || message.type === 'config') {
      if (message.protocol !== VERSION) { setStatus('error', 'unsupported protocol'); return; }
      if (typeof message.sampleRate === 'number') stream.sampleRate = message.sampleRate;
      if (typeof message.channels === 'number') stream.channels = message.channels;
      if (typeof message.hostPlaying === 'boolean') stream.hostPlaying = message.hostPlaying;
      if (message.type === 'config') ringReset();
      renderSubtitle();
    } else if (message.type === 'state') {
      stream.hostPlaying = !!message.hostPlaying;
      renderSubtitle();
    } else if (message.type === 'bye') {
      setStatus('idle', 'sender closed the stream');
    }
  }

  // ---------------------------------------------------------------------------
  // UI
  // ---------------------------------------------------------------------------
  function setStatus(kind, text) {
    el.dot.className = kind === 'live' ? 'live' : kind === 'connecting' ? 'connecting'
                     : kind === 'error' ? 'error' : '';
    el.statusText.textContent = text;
  }

  function formatName(format) {
    return format === FORMAT_PCM16 ? 'PCM16' : format === FORMAT_PCM24 ? 'PCM24' : 'float32';
  }

  function renderSubtitle() {
    if (!stats.packets) { el.subtitle.textContent = 'not connected'; return; }
    el.subtitle.textContent = location.hostname + ' · ' + (stream.sampleRate / 1000) + ' kHz · ' +
                              formatName(stream.format) + ' · ' +
                              (stream.channels > 1 ? 'stereo' : 'mono');
  }

  async function startListening() {
    if (!AudioContextClass) { setStatus('error', 'no Web Audio support'); return; }

    // One AudioContext for the lifetime of the page. Recreating it would throw away the
    // user-gesture unlock and force another tap.
    if (!ctx) {
      ctx = new AudioContextClass({ latencyHint: 'interactive' });
      gain = ctx.createGain();
      gain.connect(ctx.destination);
    }

    if ('audioSession' in navigator) {
      // Safari-only, and it is the fix for the hardware silent switch muting web audio.
      try { navigator.audioSession.type = 'playback'; } catch (e) { /* ignore */ }
    }

    if (ctx.state !== 'running') { try { await ctx.resume(); } catch (e) { /* ignore */ } }

    // Belt and braces for older iOS: a looping silent element keeps the audio session up.
    try { el.keepAlive.play().catch(function () {}); } catch (e) { /* ignore */ }

    if (!node) {
      node = createNode();
      node.connect(gain);
    }

    if (caps.wakeLock) {
      try { wakeLock = await navigator.wakeLock.request('screen'); } catch (e) { wakeLock = null; }
    }

    playing = true;
    el.listen.textContent = 'STOP';
    el.listen.classList.add('stop');
    el.noteSilent.classList.remove('hidden');
    connect();
  }

  function stopListening() {
    playing = false;
    el.listen.textContent = 'LISTEN';
    el.listen.classList.remove('stop');
    el.noteSilent.classList.add('hidden');
    if (wakeLock) { try { wakeLock.release(); } catch (e) { /* ignore */ } wakeLock = null; }
    if (socket) { socket.close(); socket = null; }
    ringReset();
    setStatus('idle', 'stopped');
  }

  el.listen.addEventListener('click', function () {
    if (playing) stopListening(); else startListening();
  });

  el.volume.addEventListener('input', function () {
    var value = Number(el.volume.value);
    el.volumeValue.textContent = value + '%';
    if (gain) gain.gain.value = value / 100;
  });

  el.buffer.addEventListener('input', function () {
    target.ms = Number(el.buffer.value);
    el.bufferValue.textContent = target.ms + ' ms';
    send({ type: 'setLatency', ms: target.ms });
  });

  document.addEventListener('visibilitychange', async function () {
    if (document.visibilityState === 'visible' && playing && caps.wakeLock && !wakeLock) {
      try { wakeLock = await navigator.wakeLock.request('screen'); } catch (e) { wakeLock = null; }
    }
  });

  el.noteWakeLock.classList.toggle('hidden', caps.wakeLock);

  // ---------------------------------------------------------------------------
  // Periodic work. Deliberately one 10 Hz timer rather than requestAnimationFrame:
  // ScriptProcessorNode runs on this same thread, and every frame spent laying out the
  // page is a frame not spent filling the audio buffer.
  // ---------------------------------------------------------------------------
  function meterWidth(value) {
    // Roughly -60 dBFS to 0 dBFS across the bar.
    if (value <= 0) return 0;
    var db = 20 * Math.log10(value);
    return Math.max(0, Math.min(100, (db + 60) * (100 / 60)));
  }

  setInterval(function () {
    var wl = meterWidth(meter.l), wr = meterWidth(meter.r);
    el.meterL.style.width = wl + '%';
    el.meterR.style.width = wr + '%';
    el.meterL.classList.toggle('hot', meter.l > 0.99);
    el.meterR.classList.toggle('hot', meter.r > 0.99);
    renderDiagnostics();
  }, 100);

  setInterval(function () {
    stats.kbitPerSecond = Math.round(stats.bytesWindow * 8 / 1000);
    stats.bytesWindow = 0;
  }, 1000);

  setInterval(function () {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    send({
      type: 'stat',
      bufferMs: Math.round(ringFill() / stream.sampleRate * 1000),
      targetMs: target.ms,
      driftPpm: Math.round(drift.ppm),
      underruns: stats.underruns,
      overruns: stats.resyncs,
      packetsReceived: stats.packets,
      packetsLost: stats.lost,
      packetsReordered: stats.reordered,
      playing: playing
    });
  }, 2000);

  function diagnosticRows() {
    var bufferMs = Math.round(ringFill() / stream.sampleRate * 1000);
    var packetMs = Math.round(stream.framesPerPacket / stream.sampleRate * 1000);
    var outputMs = ctx ? Math.round(SPN_BUFFER / ctx.sampleRate * 1000) : 0;

    return [
      ['secure context', caps.secureContext],
      ['AudioWorklet', caps.audioWorklet],
      ['SharedArrayBuffer', caps.sharedArrayBuffer],
      ['AudioDecoder', caps.audioDecoder],
      ['WebTransport', caps.webTransport],
      ['wake lock', caps.wakeLock],
      ['audio session', caps.audioSession],
      ['audio path', audioPath + (ctx ? ' @ ' + Math.round(ctx.sampleRate) + ' Hz' : '')],
      ['stream', stream.sampleRate + ' Hz · ' + stream.channels + ' ch · ' + formatName(stream.format)],
      ['packet', stream.framesPerPacket + ' frames (' + packetMs + ' ms)'],
      ['throughput', stats.kbitPerSecond + ' kbit/s · ' + stats.packets + ' packets'],
      ['buffer', bufferMs + ' ms / target ' + target.ms + ' ms'],
      ['drift', Math.round(drift.ppm) + ' ppm'],
      ['underruns / resyncs', stats.underruns + ' / ' + stats.resyncs],
      ['lost / reordered', stats.lost + ' / ' + stats.reordered],
      ['reconnects', String(stats.reconnects)],
      ['estimated latency', (packetMs + bufferMs + outputMs) + ' ms, excluding your headphones']
    ];
  }

  function renderDiagnostics() {
    if (!el.diagnostics.closest('details').open) return;

    var body = el.diagnostics.querySelector('tbody');
    var html = '';

    diagnosticRows().forEach(function (row) {
      var value = typeof row[1] === 'boolean'
        ? '<span class="' + (row[1] ? 'yes">yes' : 'no">no') + '</span>'
        : String(row[1]);
      html += '<tr><td>' + row[0] + '</td><td>' + value + '</td></tr>';
    });

    body.innerHTML = html;
  }

  el.copy.addEventListener('click', function () {
    var text = diagnosticRows().map(function (row) { return row[0] + ': ' + row[1]; }).join('\n');
    text += '\nuser agent: ' + navigator.userAgent;

    if (navigator.clipboard) navigator.clipboard.writeText(text).catch(function () {});
    el.copy.textContent = 'Copied';
    setTimeout(function () { el.copy.textContent = 'Copy diagnostics'; }, 1500);
  });

  // A one-second silent WAV, so the keep-alive element has something to loop without a
  // second network request.
  el.keepAlive.src = 'data:audio/wav;base64,UklGRiQAAABXQVZFZm10IBAAAAABAAEAgD4AAAB9AAACABAAZGF0YQAAAAA=';

  setStatus('idle', 'ready');
  renderSubtitle();
})();
