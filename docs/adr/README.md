# Architecture Decision Records

Each ADR captures one decision, the context that forced it, and the consequences we
accept by taking it. They are numbered and immutable: if a decision changes, add a
new ADR that supersedes the old one rather than editing history.

| ADR | Title | Status |
| --- | ----- | ------ |
| [0001](0001-juce-and-agpl.md) | JUCE 9 as the plugin framework, and AGPL-3.0-or-later as the licence | Accepted |
| [0002](0002-websocket-tcp-transport.md) | WebSocket over plain TCP as the audio transport | Accepted |
| [0003](0003-scriptprocessornode-and-plain-http.md) | Plain HTTP, and therefore ScriptProcessorNode with no AudioWorklet path | Accepted |
| [0004](0004-qr-code-discovery-no-mdns.md) | Discovery is a QR code — no mDNS, no Bonjour, no UDP beacon | Accepted |

These four are connected, and mostly in one direction: choosing JUCE under its free tier
sets the licence (0001); refusing to put a TLS stack inside the DAW's process gives us
plain `ws://` (0002), which means the receiver's origin is not a secure context, which
deletes AudioWorklet and the wake lock (0003); and keeping the plugin to inbound TCP only
is what keeps it out of macOS's Local Network permission regime, which rules out every
form of service discovery (0004).

Template: [`_template.md`](_template.md).
