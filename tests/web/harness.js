/*  A browser-shaped environment just large enough to load web/app.js under Node.

    The receiver page is the half of this project with the most subtle arithmetic — packet
    decoding, a ring buffer, a resampler and a drift controller — and the least natural
    test coverage. A mistake in any of them sounds like a problem with the mix rather than
    like a bug, which is the worst possible failure mode for a mix-referencing tool.

    Rather than pull in jsdom, this stubs the handful of DOM surfaces app.js actually
    touches and runs the file in a `vm` context. If app.js starts using something that is
    not stubbed, loading it throws here instead of silently misbehaving.
*/
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

function makeElement() {
  return {
    style: {},
    textContent: '',
    value: '0',
    src: '',
    innerHTML: '',
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    addEventListener() {},
    setAttribute() {},
    querySelector: () => makeElement(),
    closest: () => ({ open: false }),
    play: () => Promise.resolve()
  };
}

/** Loads web/app.js in a stubbed browser environment and returns its test hooks. */
function loadReceiver() {
  const moduleObject = { exports: {} };

  const sandbox = {
    module: moduleObject,
    console,
    performance: { now: () => Date.now() },
    navigator: { userAgent: 'node-test' },
    document: { getElementById: () => makeElement(), addEventListener() {} },
    location: { hash: '', host: '127.0.0.1:1', hostname: '127.0.0.1', protocol: 'http:' },
    WebSocket: Object.assign(function () {}, { OPEN: 1 }),
    // The page arms three timers at load; under test nothing should fire on its own.
    setInterval: () => ({ unref() {} }),
    setTimeout: () => 0,
    clearTimeout: () => {}
  };

  sandbox.window = { isSecureContext: false, AudioContext: function () {}, addEventListener() {} };
  sandbox.self = { crossOriginIsolated: false };
  sandbox.globalThis = sandbox;

  const source = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'app.js'), 'utf8');
  vm.runInNewContext(source, vm.createContext(sandbox), { filename: 'web/app.js' });

  if (!moduleObject.exports.handlePacket) {
    throw new Error('web/app.js did not export its test hooks — did the guarded export block change?');
  }

  return moduleObject.exports;
}

/** Builds a PPMX packet exactly as src/core/WirePacket.cpp would. */
function makePacket(options) {
  const {
    format = 0, channels = 2, flags = 0, sequence = 0, sampleRate = 48000,
    frames = 8, configEpoch = 0, sampleClock = 0, samples = []
  } = options;

  const bytesPerSample = format === 0 ? 2 : format === 1 ? 3 : 4;
  const buffer = new ArrayBuffer(32 + frames * channels * bytesPerSample);
  const view = new DataView(buffer);

  view.setUint8(0, 0x50); view.setUint8(1, 0x50); view.setUint8(2, 0x4d); view.setUint8(3, 0x58);
  view.setUint8(4, 1);
  view.setUint8(5, format);
  view.setUint8(6, channels);
  view.setUint8(7, flags);
  view.setUint32(8, sequence, true);
  view.setUint32(12, sampleRate, true);
  view.setUint16(16, frames, true);
  view.setUint16(18, configEpoch, true);
  view.setBigUint64(20, BigInt(sampleClock), true);
  view.setUint32(28, 0, true);

  for (let i = 0; i < frames * channels; i++) {
    const value = samples[i] === undefined ? 0 : samples[i];

    if (format === 0) view.setInt16(32 + i * 2, Math.round(value * 32767), true);
    else if (format === 1) {
      const raw = Math.round(value * 8388607) & 0xffffff;
      view.setUint8(32 + i * 3, raw & 0xff);
      view.setUint8(32 + i * 3 + 1, (raw >> 8) & 0xff);
      view.setUint8(32 + i * 3 + 2, (raw >> 16) & 0xff);
    } else view.setFloat32(32 + i * 4, value, true);
  }

  return buffer;
}

module.exports = { loadReceiver, makePacket, makeElement };
