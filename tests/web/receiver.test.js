#!/usr/bin/env node
/*  Tests for the receiver's decoding and playback arithmetic.

    Run with: node tests/web/receiver.test.js
    CI runs it as part of the test job; ctest runs it too via the `receiver` test.
*/
'use strict';

const { loadReceiver, makePacket } = require('./harness');

let failures = 0;
let current = '';

function test(name, body) {
  current = name;
  const before = failures;
  try {
    body();
  } catch (error) {
    failures++;
    console.log('  ' + error.stack.split('\n').slice(0, 3).join('\n  '));
  }
  console.log((failures === before ? '[ PASS ] ' : '[ FAIL ] ') + name);
}

function check(condition, message) {
  if (!condition) { failures++; console.log('  ' + current + ': ' + message); }
}

function checkClose(actual, expected, tolerance, message) {
  if (Math.abs(actual - expected) > tolerance) {
    failures++;
    console.log('  ' + current + ': ' + message + ' (got ' + actual + ', wanted ' + expected + ')');
  }
}

/** A ramp that is easy to recognise after a round trip through the wire format. */
function ramp(frames, channels) {
  const samples = [];
  for (let i = 0; i < frames; i++)
    for (let c = 0; c < channels; c++)
      samples.push((i / frames) * 0.5 + c * 0.25);
  return samples;
}

// ---------------------------------------------------------------------------

test('the page loads in a stubbed browser and exports its hooks', () => {
  const receiver = loadReceiver();
  check(typeof receiver.handlePacket === 'function', 'handlePacket missing');
  check(typeof receiver.pull === 'function', 'pull missing');
});

test('a pcm16 packet is decoded into the ring buffer', () => {
  const receiver = loadReceiver();
  receiver.handlePacket(makePacket({ frames: 64, samples: ramp(64, 2) }));

  check(receiver.stats.packets === 1, 'packet was not counted');
  check(receiver.ringFill() === 64, 'ring should hold 64 frames, holds ' + receiver.ringFill());
  check(receiver.stream.sampleRate === 48000, 'sample rate not latched');
  check(receiver.stream.channels === 2, 'channel count not latched');
});

test('garbage and wrong-magic packets are ignored', () => {
  const receiver = loadReceiver();

  receiver.handlePacket(new ArrayBuffer(8));                 // too short
  const wrongMagic = makePacket({ frames: 4 });
  new DataView(wrongMagic).setUint8(0, 0x00);
  receiver.handlePacket(wrongMagic);

  check(receiver.stats.packets === 0, 'a malformed packet was accepted');
  check(receiver.ringFill() === 0, 'a malformed packet reached the ring');
});

test('all three sample formats decode to the same audio', () => {
  const frames = 32;
  const samples = ramp(frames, 2);
  const results = [];

  [0, 1, 2].forEach((format) => {
    const receiver = loadReceiver();
    receiver.setContext({ sampleRate: 48000 });
    receiver.setPlaying(true);
    receiver.target.ms = 0;                                  // play immediately

    receiver.handlePacket(makePacket({ format, frames, samples }));

    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    receiver.pull(left, right, frames);
    results.push([left, right]);
  });

  // pcm16 quantises at about 3e-5, so compare against that rather than exactly.
  for (let i = 0; i < frames - 1; i++) {
    check(results[2][0][i] !== 0 || i === 0, 'float32 output went silent at frame ' + i);
    checkClose(results[0][0][i], results[2][0][i], 1e-4, 'pcm16 and float32 differ at frame ' + i);
    checkClose(results[1][0][i], results[2][0][i], 1e-5, 'pcm24 and float32 differ at frame ' + i);
    checkClose(results[2][1][i] - results[2][0][i], 0.25, 1e-5, 'channel offset lost at frame ' + i);
  }
});

test('a mono packet is played out on both channels', () => {
  const receiver = loadReceiver();
  receiver.setContext({ sampleRate: 48000 });
  receiver.setPlaying(true);
  receiver.target.ms = 0;

  receiver.handlePacket(makePacket({ format: 2, channels: 1, frames: 16, samples: ramp(16, 1) }));

  const left = new Float32Array(16);
  const right = new Float32Array(16);
  receiver.pull(left, right, 16);

  for (let i = 0; i < 16; i++)
    checkClose(left[i], right[i], 1e-9, 'mono was not duplicated at frame ' + i);
});

test('packet loss is counted and reordering is distinguished from it', () => {
  const receiver = loadReceiver();

  receiver.handlePacket(makePacket({ sequence: 10, frames: 4 }));
  receiver.handlePacket(makePacket({ sequence: 14, frames: 4 }));   // three missing
  check(receiver.stats.lost === 3, 'expected 3 lost, got ' + receiver.stats.lost);

  receiver.handlePacket(makePacket({ sequence: 13, frames: 4 }));   // arrives late
  check(receiver.stats.reordered === 1, 'expected 1 reordered, got ' + receiver.stats.reordered);
});

test('a discontinuity flag flushes whatever was buffered', () => {
  const receiver = loadReceiver();

  receiver.handlePacket(makePacket({ sequence: 1, frames: 64, samples: ramp(64, 2) }));
  check(receiver.ringFill() === 64, 'setup failed');

  receiver.handlePacket(makePacket({ sequence: 2, frames: 64, flags: 1, samples: ramp(64, 2) }));
  check(receiver.ringFill() === 64, 'discontinuity should have flushed the old audio first, '
                                    + 'leaving only the new packet, but the ring holds '
                                    + receiver.ringFill());
});

test('a config epoch change resets the pipeline', () => {
  const receiver = loadReceiver();

  receiver.handlePacket(makePacket({ frames: 64, configEpoch: 0, samples: ramp(64, 2) }));
  receiver.handlePacket(makePacket({ frames: 64, configEpoch: 1, sampleRate: 44100, samples: ramp(64, 2) }));

  check(receiver.stream.sampleRate === 44100, 'new sample rate not adopted');
  check(receiver.ringFill() === 64, 'old audio survived a config change');
});

test('the ring discards down to target instead of growing without bound', () => {
  const receiver = loadReceiver();
  receiver.target.ms = 20;                                   // 960 frames at 48 kHz

  // A backgrounded tab stops draining. TCP will happily hand us minutes of backlog;
  // nobody wants to hear it, so the ring must throw it away rather than queue it.
  for (let i = 0; i < 40; i++)
    receiver.handlePacket(makePacket({ sequence: i, frames: 512, samples: ramp(512, 2) }));

  check(receiver.ringFill() <= receiver.targetFrames() * 3 + 512,
        'ring grew to ' + receiver.ringFill() + ' frames, past the 3x target limit');
  check(receiver.stats.resyncs > 0, 'discarding should have been counted as a resync');
});

test('resampling from 48 kHz to 44.1 kHz consumes more input than it emits', () => {
  const receiver = loadReceiver();
  receiver.setContext({ sampleRate: 44100 });
  receiver.setPlaying(true);
  receiver.target.ms = 0;

  for (let i = 0; i < 8; i++)
    receiver.handlePacket(makePacket({ sequence: i, frames: 512, samples: ramp(512, 2) }));

  const before = receiver.ringFill();
  const left = new Float32Array(441);
  const right = new Float32Array(441);
  receiver.pull(left, right, 441);
  const consumed = before - receiver.ringFill();

  // 441 output frames at 44.1 kHz is 10 ms, which is 480 input frames at 48 kHz.
  checkClose(consumed, 480, 3, 'wrong number of input frames consumed');
});

test('an underrun fades instead of clicking', () => {
  const receiver = loadReceiver();
  receiver.setContext({ sampleRate: 48000 });
  receiver.setPlaying(true);
  receiver.target.ms = 0;

  // 64 frames of steady signal, then nothing: the remaining 192 have to be concealed.
  receiver.handlePacket(makePacket({ format: 2, frames: 64, samples: new Array(128).fill(0.5) }));

  const left = new Float32Array(256);
  const right = new Float32Array(256);
  receiver.pull(left, right, 256);

  check(receiver.stats.underruns === 1, 'underrun was not counted');

  // The audio the sender provided must come out intact...
  for (let i = 0; i < 60; i++)
    checkClose(left[i], 0.5, 1e-5, 'buffered audio altered at frame ' + i);

  // ...and past the end of it, every step has to be small. A jump to zero is exactly the
  // click this concealment exists to prevent.
  for (let i = 1; i < 256; i++)
    check(Math.abs(left[i] - left[i - 1]) < 0.01,
          'discontinuity of ' + (left[i] - left[i - 1]).toFixed(4) + ' at frame ' + i);

  // And it has to actually decay, not hold a tone forever.
  check(Math.abs(left[255]) < 0.45, 'concealment did not fade, ended at ' + left[255]);
});

test('the drift correction stays inside its clamp', () => {
  const receiver = loadReceiver();
  receiver.setContext({ sampleRate: 48000 });
  receiver.setPlaying(true);
  receiver.target.ms = 100;

  // Starve it hard and for a long time; the controller must not respond with an audible
  // pitch bend, only with a fraction of a cent.
  for (let i = 0; i < 200; i++) {
    receiver.pull(new Float32Array(512), new Float32Array(512), 512);
    check(Math.abs(receiver.drift.correction) <= 0.002 + 1e-12,
          'correction ' + receiver.drift.correction + ' exceeded the 0.2% clamp');
  }
});

test('the jitter buffer is never allowed below two output blocks', () => {
  const receiver = loadReceiver();
  receiver.setContext({ sampleRate: 48000 });
  receiver.refreshFloor();

  // ScriptProcessorNode takes a whole 4096-frame block in one instant, so the real margin
  // against network jitter is target minus block duration. Anything below two blocks
  // cannot survive ordinary Wi-Fi, and the slider must not be able to ask for it.
  check(receiver.minimumTargetMs() >= 170, 'floor is only ' + receiver.minimumTargetMs() + ' ms');

  receiver.target.ms = 40;
  check(receiver.effectiveTargetMs() === receiver.minimumTargetMs(),
        'a 40 ms request was not clamped to the floor');

  receiver.target.ms = 400;
  check(receiver.effectiveTargetMs() === 400, 'a request above the floor was not honoured');
});

test('bursty delivery does not quietly fade the audio away', () => {
  // The regression this exists for: at the old 120 ms default, an 85 ms output block left
  // 35 ms of margin — less than the p99 delivery tail on Wi-Fi. The buffer emptied on
  // almost every burst and concealment faded each block out, so the stream lost 7.6 dB
  // and sounded like a thin, gasping mix rather than like a dropout. Concealment was
  // hiding the fault instead of reporting it.
  const receiver = loadReceiver();
  receiver.setContext({ sampleRate: 48000 });
  receiver.setPlaying(true);
  receiver.refreshFloor();

  const RATE = 48000, PACKET = 512, BLOCK = 4096, AMPLITUDE = 0.5;
  let phase = 0, sequence = 0, owed = 0, seed = 12345;
  const rand = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);

  function deliverPacket() {
    const samples = [];
    for (let i = 0; i < PACKET; i++) {
      const value = AMPLITUDE * Math.sin(2 * Math.PI * 440 * phase / RATE);
      phase++;
      samples.push(value, value);
    }
    receiver.handlePacket(makePacket({ format: 2, frames: PACKET, sequence: sequence++, samples }));
  }

  const prime = Math.ceil(receiver.effectiveTargetMs() * 0.001 * RATE / PACKET) + BLOCK / PACKET;
  for (let i = 0; i < prime; i++) deliverPacket();

  const left = new Float32Array(BLOCK);
  const right = new Float32Array(BLOCK);
  let sumOfSquares = 0, count = 0;

  for (let block = 0; block < 200; block++) {
    receiver.pull(left, right, BLOCK);

    for (let i = 0; i < BLOCK; i++) { sumOfSquares += left[i] * left[i]; count++; }

    // Every fifth burst or so is delayed and arrives late, which is what an access point
    // under load actually does.
    owed += BLOCK / PACKET;
    if (rand() >= 0.4) { while (owed >= 1) { deliverPacket(); owed -= 1; } }
  }

  const rms = Math.sqrt(sumOfSquares / count);
  const expected = AMPLITUDE / Math.SQRT2;
  const lossDb = 20 * Math.log10(rms / expected);

  check(lossDb > -1.5, 'lost ' + lossDb.toFixed(1) + ' dB to bursty delivery (was -7.6 dB before the fix)');
});

// ---------------------------------------------------------------------------
console.log('');
console.log(failures === 0 ? 'receiver tests passed' : failures + ' receiver check(s) failed');
process.exit(failures === 0 ? 0 : 1);
