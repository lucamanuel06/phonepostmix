#!/usr/bin/env node
/*  PhonePostMix headless receiver.

    Connects to a running PhonePostMix plugin, decodes the audio stream and reports what
    it sees. It exists so the sender can be developed and tested without a phone in your
    hand, and so a bug report can include real numbers instead of "it sounded wrong".

    No dependencies: the WebSocket client is ~80 lines over `net` and `crypto`, which is
    less code than adding a package.json would be.

    Usage:
      node tools/listen.js <url>            # url as shown in the plugin, token included
      node tools/listen.js <url> --wav out.wav --seconds 10

    Example:
      node tools/listen.js 'http://192.168.1.50:17520/#t=abc123'
*/
'use strict';

const net = require('net');
const crypto = require('crypto');
const fs = require('fs');

const HEADER_BYTES = 32;
const MAGIC = 0x584d5050;
const FORMAT_NAMES = { 0: 'pcm16', 1: 'pcm24', 2: 'float32' };

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------
function parseArguments(argv) {
  const options = { url: null, wav: null, seconds: 0 };

  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--wav') options.wav = argv[++i];
    else if (argv[i] === '--seconds') options.seconds = Number(argv[++i]);
    else if (!options.url) options.url = argv[i];
  }

  return options;
}

const options = parseArguments(process.argv.slice(2));

if (!options.url) {
  console.error('usage: node tools/listen.js <url> [--wav out.wav] [--seconds N]');
  process.exit(2);
}

let parsed;
try {
  parsed = new URL(options.url);
} catch (e) {
  console.error('not a valid URL: ' + options.url);
  process.exit(2);
}

// The token lives in the fragment of the URL the plugin shows, exactly as it does for the
// browser page, and moves onto the WebSocket query string here.
const token = (parsed.hash.match(/[#&]t=([0-9a-fA-F]+)/) || [])[1] || '';
const host = parsed.hostname;
const port = Number(parsed.port || 80);

// ---------------------------------------------------------------------------
// Minimal WebSocket client
// ---------------------------------------------------------------------------
function connect(onOpen, onText, onBinary, onClose) {
  const key = crypto.randomBytes(16).toString('base64');
  const accept = crypto.createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');

  const socket = net.connect(port, host, () => {
    socket.write(
      'GET /ws' + (token ? '?t=' + encodeURIComponent(token) : '') + ' HTTP/1.1\r\n' +
      'Host: ' + host + ':' + port + '\r\n' +
      'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
      'Sec-WebSocket-Key: ' + key + '\r\n' +
      'Sec-WebSocket-Version: 13\r\n\r\n');
  });

  let buffer = Buffer.alloc(0);
  let upgraded = false;

  socket.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);

    if (!upgraded) {
      const end = buffer.indexOf('\r\n\r\n');
      if (end < 0) return;

      const head = buffer.slice(0, end).toString('latin1');
      buffer = buffer.slice(end + 4);

      if (!/^HTTP\/1\.1 101 /.test(head)) {
        const status = head.split('\r\n')[0];
        socket.destroy();
        onClose('handshake refused: ' + status +
                (status.indexOf('403') >= 0 ? ' — wrong or missing token' : ''));
        return;
      }

      if (head.indexOf('Sec-WebSocket-Accept: ' + accept) < 0) {
        socket.destroy();
        onClose('handshake accept key mismatch');
        return;
      }

      upgraded = true;
      onOpen(socket);
    }

    // Server frames are never masked and this server never fragments, so the reader
    // only has to handle the three length encodings.
    for (;;) {
      if (buffer.length < 2) return;

      const opcode = buffer[0] & 0x0f;
      let size = buffer[1] & 0x7f;
      let offset = 2;

      if (size === 126) {
        if (buffer.length < 4) return;
        size = buffer.readUInt16BE(2);
        offset = 4;
      } else if (size === 127) {
        if (buffer.length < 10) return;
        size = Number(buffer.readBigUInt64BE(2));
        offset = 10;
      }

      if (buffer.length < offset + size) return;

      const payload = buffer.slice(offset, offset + size);
      buffer = buffer.slice(offset + size);

      if (opcode === 0x1) onText(payload.toString('utf8'));
      else if (opcode === 0x2) onBinary(payload);
      else if (opcode === 0x8) { socket.end(); onClose('server closed the stream'); return; }
    }
  });

  socket.on('error', (error) => onClose(error.message));
  socket.on('close', () => onClose('disconnected'));

  return socket;
}

function sendText(socket, text) {
  const payload = Buffer.from(text, 'utf8');
  const mask = crypto.randomBytes(4);
  const header = payload.length < 126
    ? Buffer.from([0x81, 0x80 | payload.length])
    : Buffer.concat([Buffer.from([0x81, 0x80 | 126]),
                     Buffer.from([payload.length >> 8, payload.length & 0xff])]);

  const masked = Buffer.alloc(payload.length);
  for (let i = 0; i < payload.length; i++) masked[i] = payload[i] ^ mask[i % 4];

  socket.write(Buffer.concat([header, mask, masked]));
}

// ---------------------------------------------------------------------------
// Packet decoding
// ---------------------------------------------------------------------------
const state = {
  packets: 0, bytes: 0, lost: 0, lastSeq: -1, peak: 0,
  sampleRate: 0, channels: 0, format: -1, frames: 0, epoch: -1, started: Date.now()
};

const wavChunks = [];

function decodePacket(payload) {
  if (payload.length < HEADER_BYTES) return;
  if (payload.readUInt32LE(0) !== MAGIC) return;

  const version = payload.readUInt8(4);
  if (version !== 1) { console.error('unsupported protocol version ' + version); process.exit(1); }

  const format = payload.readUInt8(5);
  const channels = payload.readUInt8(6);
  const seq = payload.readUInt32LE(8);
  const sampleRate = payload.readUInt32LE(12);
  const frames = payload.readUInt16LE(16);
  const epoch = payload.readUInt16LE(18);

  if (epoch !== state.epoch) {
    state.epoch = epoch;
    console.log('stream: ' + sampleRate + ' Hz · ' + channels + ' ch · ' +
                (FORMAT_NAMES[format] || format) + ' · ' + frames + ' frames/packet');
  }

  if (state.lastSeq >= 0) {
    const delta = (seq - state.lastSeq) >>> 0;
    if (delta > 1 && delta < 0x80000000) state.lost += delta - 1;
  }

  state.lastSeq = seq;
  state.packets++;
  state.bytes += payload.length;
  state.sampleRate = sampleRate;
  state.channels = channels;
  state.format = format;
  state.frames = frames;

  const samples = frames * channels;
  const body = payload.slice(HEADER_BYTES);

  for (let i = 0; i < samples; i++) {
    let value;
    if (format === 0) value = body.readInt16LE(i * 2) / 32767;
    else if (format === 1) {
      const raw = body[i * 3] | (body[i * 3 + 1] << 8) | (body[i * 3 + 2] << 16);
      value = (raw & 0x800000 ? raw - 0x1000000 : raw) / 8388607;
    } else value = body.readFloatLE(i * 4);

    const magnitude = Math.abs(value);
    if (magnitude > state.peak) state.peak = magnitude;
  }

  if (options.wav) wavChunks.push(Buffer.from(body));
}

function writeWav(path) {
  if (!wavChunks.length) { console.error('no audio captured, not writing ' + path); return; }

  const data = Buffer.concat(wavChunks);
  const bits = state.format === 0 ? 16 : state.format === 1 ? 24 : 32;
  const isFloat = state.format === 2;
  const blockAlign = state.channels * (bits / 8);

  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + data.length, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(isFloat ? 3 : 1, 20);
  header.writeUInt16LE(state.channels, 22);
  header.writeUInt32LE(state.sampleRate, 24);
  header.writeUInt32LE(state.sampleRate * blockAlign, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(bits, 34);
  header.write('data', 36);
  header.writeUInt32LE(data.length, 40);

  fs.writeFileSync(path, Buffer.concat([header, data]));
  console.log('wrote ' + path + ' (' + (data.length / blockAlign / state.sampleRate).toFixed(2) + ' s)');
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
let live = null;
let finished = false;

function finish(reason, code) {
  if (finished) return;
  finished = true;

  console.log('\n' + reason);
  console.log('packets ' + state.packets + ' · lost ' + state.lost +
              ' · peak ' + (state.peak > 0 ? (20 * Math.log10(state.peak)).toFixed(1) + ' dBFS' : 'silence'));

  if (options.wav) writeWav(options.wav);
  process.exit(code);
}

live = connect(
  (socket) => {
    console.log('connected to ' + host + ':' + port);
    sendText(socket, JSON.stringify({ type: 'ready', protocol: 1, path: 'node', ua: 'tools/listen.js' }));

    if (options.seconds > 0) setTimeout(() => { socket.end(); finish('done', 0); }, options.seconds * 1000);
  },
  (text) => {
    try {
      const message = JSON.parse(text);
      if (message.type === 'hello') console.log('sender: ' + message.sender);
      if (message.type === 'bye') finish('sender closed the stream', 0);
    } catch (e) { /* ignore anything that is not JSON we understand */ }
  },
  decodePacket,
  (reason) => finish(reason, state.packets > 0 ? 0 : 1)
);

setInterval(() => {
  if (!state.packets) return;
  const seconds = (Date.now() - state.started) / 1000;
  process.stdout.write('\r' + state.packets + ' packets · ' +
    Math.round(state.bytes * 8 / 1000 / seconds) + ' kbit/s · lost ' + state.lost +
    ' · peak ' + (state.peak > 0 ? (20 * Math.log10(state.peak)).toFixed(1) : '-inf') + ' dBFS   ');
}, 500).unref();

process.on('SIGINT', () => finish('interrupted', 0));
