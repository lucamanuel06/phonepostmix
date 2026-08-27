# The PhonePostMix wire protocol

Version 1. This document describes everything a receiver has to know. It is written from
the implementation, not from the design notes: `src/core/WirePacket.{h,cpp}`,
`src/core/StreamEngine.cpp`, `src/core/StreamServer.cpp`, `src/core/HttpMessage.cpp` and
`web/app.js`. `tools/listen.js` is a complete working receiver in 300 lines of dependency-free
JavaScript and is the shortest way to see the whole thing at once.

Design constraints worth knowing before you read the details:

- Everything is **little-endian**. No exceptions, no byte-order negotiation.
- Every audio packet is **self-describing**. Format, channel count, sample rate and frame
  count are repeated in every single packet, not just in a handshake, because receivers
  join mid-stream, and hosts change sample rate underneath a running plugin.
- The transport is **plain TCP** — HTTP/1.1 then RFC 6455 WebSocket, no TLS.
- Unknown JSON message types and unknown keys are **ignored, never rejected**, on both
  sides. That is the entire forward-compatibility contract.

## 1. Transport and endpoints

The plugin binds one TCP port on `0.0.0.0`. The preferred port is **17520**; if it is
taken, the server tries the next 19 ports (17520–17539) and gives up after that. The bound
port is shown in the editor and is in the listen URL.

The server is HTTP/1.1, but a small subset of it: no request bodies, no chunked transfer,
no keep-alive, no ranges. Every non-WebSocket response ends with the connection being
closed. A request head larger than 16384 bytes closes the connection with no response.

### HTTP endpoints

| Method | Path | Response |
| --- | --- | --- |
| `GET` | `/` | `200 OK`, `Content-Type: text/html; charset=utf-8`, the receiver page. |
| `GET` | `/app.js` | `200 OK`, `Content-Type: text/javascript; charset=utf-8`, the receiver script. |
| `GET` | anything else | `404 Not Found`, `text/plain; charset=utf-8`, body `Not found`. |

Both assets are compiled into the plugin binary; there is no filesystem behind them and no
path to traverse. Successful asset responses carry `Content-Length`, `Connection: close`
and `Cache-Control: no-store` — the page is rebuilt with every plugin build, and a cached
copy from an older version would silently mismatch the wire protocol.

There is **no** `/api/session` endpoint, no `/worklet.js`, and no other route. Session
information arrives over the WebSocket instead (§4).

### WebSocket upgrade

A request is treated as a WebSocket upgrade when **all** of the following hold
(`http::Request::isWebSocketUpgrade`):

- the method is `GET`;
- the `Upgrade` header's comma-separated token list contains `websocket` (case-insensitive);
- the `Connection` header's token list contains `upgrade` (case-insensitive);
- `Sec-WebSocket-Key` is present and non-empty.

Note what is *not* checked: **the path is irrelevant.** The receiver page and
`tools/listen.js` both use `/ws?t=…` by convention, and you should too, but the server
upgrades any path that carries those headers. `Sec-WebSocket-Version`, `Origin` and
requested subprotocols are ignored, and no subprotocol is negotiated.

The token check (§2) runs first. On success the server replies:

```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: <base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))>

```

No `Content-Type`, no `Content-Length`.

### Framing rules

Standard RFC 6455, with these implementation facts you may rely on:

- **Server → client frames are never masked and never fragmented.** A receiver only needs
  to handle the three payload-length encodings (7-bit, 16-bit, 64-bit).
- Audio arrives in **binary** frames (opcode `0x2`), one packet per frame. Control
  messages arrive in **text** frames (opcode `0x1`) as UTF-8 JSON.
- **Client → server frames must be masked**, as the RFC requires. An unmasked frame is a
  protocol error and the connection is closed. Fragmented client messages are reassembled
  before delivery; interleaved control frames are handled immediately.
- A client payload — or a reassembled client message — larger than **1 MiB** closes the
  connection.
- The server answers `ping` (`0x9`) with a `pong` (`0xA`) carrying the same payload. It
  never initiates a ping itself, so a receiver that wants a keepalive must send its own.
- A `close` (`0x8`) from the client ends the connection. **The server does not send a
  close frame**: on shutdown it closes the TCP socket, so a receiver sees a disconnect,
  not a clean close. Reconnect logic should treat any disconnect the same way.
- Client `binary` and `pong` frames are read and discarded.

### Backpressure

Each connected client has its own thread and its own outgoing queue, capped at 2 MiB by
default. When a client stops draining — a phone on a congested access point — the server
drops the **oldest** audio frames until the queue fits, and counts the drops. Control
frames are never dropped. A write that cannot make progress for 5 seconds is treated as a
dead peer and the connection is closed.

The consequence for a receiver: a gap in the sequence number over TCP means *the sender
dropped packets under backpressure*, not that the network lost them. Either way you have a
hole to conceal.

## 2. The pairing token

When the user presses start, the plugin generates a token: four 32-bit values from JUCE's
`Random`, hex-encoded and concatenated. Up to 128 bits; the hex is not zero-padded, so the
string is between about 16 and 32 lowercase hex characters. It lives for as long as the
streaming session and is not rotated.

The listen URL puts it in the **fragment**:

```
http://192.168.1.50:17520/#t=1f3c9ab27d5e4180a0b3c6d9e2f4a851
```

A fragment is never sent to a server, so the token stays out of request lines, server logs
and `Referer` headers. The page (or your receiver) extracts it and moves it onto the
WebSocket **query string**:

```
GET /ws?t=1f3c9ab27d5e4180a0b3c6d9e2f4a851 HTTP/1.1
```

The server parses the query string of the upgrade target, looks for the first parameter
named exactly `t`, and compares its value to the token with an exact string comparison. Any
other outcome — no query string, no `t`, a mismatched value — is a rejection:

```
HTTP/1.1 403 Forbidden
Content-Type: text/plain; charset=utf-8
Content-Length: 9
Connection: close

Forbidden
```

The token gates **only** the WebSocket upgrade. `GET /` and `GET /app.js` are served to
anyone who asks; they contain no secrets. Nothing is encrypted, so anyone who can capture
the Wi-Fi traffic can read the token off the handshake.

## 3. Audio packets

One binary WebSocket frame carries exactly one packet: a 32-byte header followed by
interleaved samples. A packet is never split across frames and frames never carry more
than one packet.

```
packetSize = 32 + frames × channels × bytesPerSample(format)
```

### Header layout

All multi-byte fields are little-endian.

| Offset | Size | Type | Field | Notes |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | bytes | `magic` | ASCII `P`,`P`,`M`,`X` (`0x50 0x50 0x4D 0x58`). Read as a little-endian `uint32` this is `0x584D5050`. Drop any packet that does not start with it. |
| 4 | 1 | `uint8` | `version` | Protocol version, currently **1**. A receiver must refuse a version it does not know and say so — do not attempt to parse it. |
| 5 | 1 | `uint8` | `format` | `0` = pcm16, `1` = pcm24, `2` = float32. See §3.2. |
| 6 | 1 | `uint8` | `channels` | 1 or 2 in practice. Samples are interleaved. |
| 7 | 1 | `uint8` | `flags` | Bit field, §3.1. |
| 8 | 4 | `uint32` | `sequence` | Increments by one per packet, wraps at 2³². Not reset on discontinuity. |
| 12 | 4 | `uint32` | `sampleRate` | Hz, as reported by the host, truncated to an integer. |
| 16 | 2 | `uint16` | `frames` | Frames in this packet's payload. 256, 512 or 1024 in the current sender. |
| 18 | 2 | `uint16` | `configEpoch` | Bumped whenever the stream configuration changes, §3.3. |
| 20 | 8 | `uint64` | `sampleClock` | Absolute frame index of this packet's first frame. Reset to 0 on a discontinuity. |
| 28 | 4 | `uint32` | `senderTimeMs` | Low 32 bits of the sender's millisecond counter when the packet was built. **Diagnostics only** — no defined epoch, not synchronised with anything, do not compute latency from it. |
| 32 | … | | payload | `frames × channels` interleaved samples. |

The header is 32 bytes rather than the 30 the fields need for one reason: the payload
offset must be a multiple of 4 so a receiver can build a `Float32Array` view directly over
the packet buffer for the float32 format without copying.

### 3.1 Flags

| Bit | Value | Name | Meaning |
| --- | --- | --- | --- |
| 0 | `0x01` | `discontinuity` | The sample clock restarted. Flush your jitter buffer and reprime. Set on the first packet of a stream, after a `prepareToPlay`, and after a format or packet-size change. |
| 1 | `0x02` | `silence` | The host's transport is **not rolling**. |
| 2 | `0x04` | `configChanged` | First packet of a new `configEpoch`. In the current sender this is always set together with `discontinuity`. |

⚠️ `silence` is a *transport state*, not a promise about the payload. The sender sets it
whenever the host reports that playback is stopped, but the payload still contains whatever
was on the master bus — which is real audio if something is making sound while stopped.
Do not skip decoding a packet because of this flag. Both shipped receivers ignore it.

### 3.2 Sample formats

Samples are interleaved by frame: `L R L R …` for stereo.

| `format` | Name | Bytes/sample | Encoding |
| --- | --- | --- | --- |
| 0 | `pcm16` | 2 | Signed 16-bit little-endian. Sender clamps to [-1, 1] and scales by **32767**; decode by dividing by 32767. |
| 1 | `pcm24` | 3 | Signed 24-bit little-endian, three bytes low-to-high, two's complement. Sender clamps to [-1, 1] and scales by **8388607**; decode by sign-extending bit 23 and dividing by 8388607. |
| 2 | `float32` | 4 | IEEE 754 single precision, little-endian, passed through untouched — values outside [-1, 1] survive, so you can see exactly what the DAW sent, overs included. |

The 32767/8388607 scaling (rather than 32768/8388608) is deliberate: scaling by 32768 maps
+1.0 to −32768 and produces a full-scale inversion click on every over.

Bandwidth at 48 kHz stereo: pcm16 ≈ 1.5 Mbit/s, pcm24 ≈ 2.3 Mbit/s, float32 ≈ 3.1 Mbit/s,
plus a few percent of framing overhead.

### 3.3 Configuration changes

`configEpoch` increments whenever the stream description changes: a host sample-rate or
channel-count change (via `prepareToPlay`), or the user changing the format or packet size.
The first packet of the new epoch carries `discontinuity | configChanged`, and a `config`
control message (§4) is broadcast as well — but the control message and the packet are not
synchronised, and a client that connects mid-stream may never see one for the epoch it
joins.

**A receiver must therefore treat the packet header as authoritative.** The reference
receiver compares `configEpoch`, `sampleRate`, `channels` and `format` from every packet
against what it currently believes, and resets its ring buffer if any of them differ. Do
the same and you never need the control message at all.

## 4. Control messages

JSON objects in WebSocket text frames, UTF-8. Every message has a `type`. Unknown types
and unknown keys are ignored on both sides.

Only the messages listed here exist. In particular there is **no** `bye` and no `state`
message: `web/app.js` and `tools/listen.js` both contain handlers for them, but the sender
never emits either, so those branches are dead. Do not implement them in a new receiver.

### 4.1 Sender → receiver

Two message types, with an identical body (`StreamEngine::makeSessionJson`):

- **`hello`** — broadcast when *any* client completes an upgrade. Note that it is a
  broadcast, not a unicast: every connected client sees a `hello` when a new one joins.
- **`config`** — broadcast when the configuration changes: from `prepareToPlay`, and when
  the user changes format or packet size. The reference receiver resets its ring buffer on
  `config` but not on `hello`.

```json
{
  "type": "hello",
  "protocol": 1,
  "sender": "PhonePostMix 0.1.0",
  "headerBytes": 32,
  "format": "pcm16",
  "sampleRate": 48000,
  "channels": 2,
  "framesPerPacket": 512,
  "configEpoch": 3,
  "suggestedTargetLatencyMs": 120,
  "hostPlaying": false
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `protocol` | number | Wire protocol version; matches the header's `version` byte. Refuse anything but `1`. |
| `sender` | string | Human-readable sender name and version. Display only. |
| `headerBytes` | number | Size of the packet header, 32. |
| `format` | string | `"pcm16"`, `"pcm24"` or `"float32"` — the *name*, not the numeric code the packet header uses. |
| `sampleRate` | number | Hz. |
| `channels` | number | 1 or 2. |
| `framesPerPacket` | number | Frames per packet the sender is currently producing. |
| `configEpoch` | number | Current epoch; matches the header field. |
| `suggestedTargetLatencyMs` | number | The sender's suggested jitter-buffer target. Advisory; the receiver owns the actual value. |
| `hostPlaying` | boolean | Whether the host transport was rolling when the message was built. Not pushed on change — it is only accurate at the moment the message was sent. |

That is the complete set. There is no periodic heartbeat and no server-initiated ping.

### 4.2 Receiver → sender

Both are optional: a receiver that sends nothing at all still gets audio. They exist so
the plugin's editor can show who is listening and how well it is going.

**`ready`** — send once after the socket opens.

```json
{
  "type": "ready",
  "protocol": 1,
  "ctxSampleRate": 48000,
  "path": "spn",
  "caps": { "secureContext": false, "audioWorklet": false },
  "targetLatencyMs": 120,
  "ua": "Mozilla/5.0 (iPhone; …)"
}
```

The sender reads exactly two fields and ignores the rest:

| Field | Used for |
| --- | --- |
| `ua` | Shown in the editor's client list. |
| `path` | Shown in the editor's client list — the receiver's audio path. The page sends `"spn"`; `tools/listen.js` sends `"node"`. Any string is accepted. |

**`stat`** — send periodically. The page sends one every 2 seconds.

```json
{
  "type": "stat",
  "bufferMs": 118,
  "targetMs": 120,
  "driftPpm": -37,
  "underruns": 0,
  "overruns": 1,
  "packetsReceived": 4180,
  "packetsLost": 0,
  "packetsReordered": 0,
  "playing": true
}
```

The sender reads `bufferMs`, `targetMs`, `underruns`, `driftPpm` and `playing`, and
displays them per client in the editor. `overruns`, `packetsReceived`, `packetsLost` and
`packetsReordered` are sent by the page but currently ignored by the sender.

**`setLatency`** — sent by the page whenever the buffer slider moves:

```json
{ "type": "setLatency", "ms": 80 }
```

⚠️ **The sender has no handler for this message and silently ignores it.** The jitter
buffer lives entirely on the receiver, so the slider still works locally; the message is
sent in the expectation that the sender will one day echo it into the editor. Do not
expect any response, and do not rely on it having an effect.

## 5. Writing a receiver: the short version

1. Read the token out of the URL fragment (`#t=…`).
2. Open a WebSocket to `ws://<host>:<port>/ws?t=<token>`. A 403 means a wrong or missing
   token; anything other than `101` means give up rather than retry.
3. Optionally send `{"type":"ready", …}`.
4. For each **text** frame: parse JSON, ignore what you do not recognise. Check `protocol`
   on `hello`/`config` and refuse anything but `1`.
5. For each **binary** frame:
   - reject it if it is shorter than 32 bytes, if the magic is not `PPMX`, or if the
     version is not 1;
   - if `configEpoch`, `sampleRate`, `channels` or `format` differ from what you have,
     reset your buffer and adopt the new values;
   - if `discontinuity` is set, reset your buffer;
   - track `sequence` for loss (a gap means the sender dropped frames under backpressure);
   - decode `frames × channels` interleaved samples per §3.2 and push them into your
     jitter buffer.
6. Play out at your own clock. Your sound card's rate will not exactly match the DAW's, so
   resample by `sampleRate / outputRate` and correct slowly — the reference receiver uses a
   PI controller on ring fill clamped to ±0.2 %, and resynchronises abruptly if it stays
   pinned at the clamp for 5 seconds.
7. On disconnect, reconnect with backoff. The server does not send a close frame, so treat
   every disconnect as unclean.

`tools/listen.js` implements steps 1–5 and 7 in a form you can read in one sitting.
