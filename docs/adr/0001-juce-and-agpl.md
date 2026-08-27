# ADR-0001: JUCE 9 as the plugin framework, and AGPL-3.0-or-later as the licence

- **Status:** Accepted
- **Date:** 2026-08-27

## Context

PhonePostMix has to load as an audio plugin inside commercial DAWs, in at least the AU and
VST3 formats, on at least macOS. Writing the plugin wrappers by hand means implementing
Steinberg's VST3 SDK and Apple's Audio Unit API directly, plus a GUI toolkit that works
inside a host window on every platform. That is months of work before a single audio sample
moves, and it is work that has already been done well.

JUCE is the framework the industry already uses for exactly this. Beyond the plugin
wrappers, we need several things it happens to bring with it: `juce::AbstractFifo` for the
lock-free ring buffer, `juce::StreamingSocket` for a TCP server that does not drag in
another library, `juce_add_binary_data` to compile the receiver page into the binary,
`juce::IPAddress::findAllAddresses` for the interface picker, and Base64 for the WebSocket
handshake. The CMake API (`juce_add_plugin`, `FetchContent`) means a fresh clone builds all
three formats with nothing installed but CMake and a compiler.

The catch is licensing, and it is not negotiable. **JUCE 9's modules are dual-licensed:
AGPLv3, or a paid commercial licence.** There is no permissive tier and no "free for small
projects" tier that avoids copyleft. Linking JUCE under the free tier and distributing the
result means the distributed work must be AGPLv3 as well. This is the single most common
licensing surprise for people new to JUCE, and it is load-bearing here: AGPLv3 §13 extends
the source-offer obligation to users who interact with the software **remotely over a
network** — which is a precise description of what this plugin does when it serves a page
to a phone. See [`../PLAN-risks.md`](../PLAN-risks.md) R21.

## Decision

Build on **JUCE 9.0.1**, fetched at configure time via `FetchContent` and pinned to a tag,
and license PhonePostMix as **AGPL-3.0-or-later**.

## Consequences

What becomes easy:

- One `cmake -B build && cmake --build build` produces AU, VST3 and Standalone, copied into
  the user's plug-in folders, on three platforms.
- The lock-free FIFO, the socket layer, the binary-data embedding, the interface
  enumeration and Base64 are all solved problems we do not maintain.
- A fresh clone needs only CMake and a compiler; `-DPPM_JUCE_PATH` reuses an existing
  checkout when the download is unwelcome.
- Pinning to a tag means the build does not break next month because upstream moved.

What becomes hard, and what we give up:

- **Anyone who distributes a build must publish the corresponding source under AGPLv3.**
  That rules out a closed-source commercial release of this codebase without buying a JUCE
  licence, and it rules out a contributor who cannot accept those terms. `CONTRIBUTING.md`
  states it up front.
- **§13 applies to the served page.** Offering the source to the person holding the phone
  is trivially satisfiable — one link on the receiver page pointing at the tag that built
  the binary — and the page does not have one yet. That is a known gap, recorded in the
  README.
- The build downloads a few hundred MB on first configure unless `PPM_JUCE_PATH` is set.
- We inherit JUCE's bugs, including the ones in `StreamingSocket` that
  [ADR-0002](0002-websocket-tcp-transport.md) and the acceptor's polling loop work around.
- macOS universal binaries double build time, so they are opt-in
  (`-DPPM_UNIVERSAL_BINARY=ON`) rather than default.

## Alternatives considered

- **Buy a JUCE commercial licence.** Removes the copyleft obligation entirely, and is the
  right answer for a closed-source product. Rejected because this is an open-source
  project with no revenue: the licence cost buys us nothing we want, and AGPLv3 is a
  licence we are happy to ship under.
- **iPlug2 (or another permissively licensed framework).** MIT/BSD-style, would leave us
  free to choose any licence. Rejected because the ecosystem, documentation and
  battle-testing are not comparable, and because we would still need to solve the FIFO,
  the socket layer and the binary-data embedding ourselves. The licence freedom is not
  worth the engineering.
- **Hand-written VST3 and AU wrappers, no framework.** Maximum control, no copyleft from a
  framework. Rejected as months of work in exactly the part of the problem that is least
  interesting and most thoroughly solved.
- **JUCE 8 or older, hoping for gentler terms.** Rejected: the GPLv3/AGPLv3-or-commercial
  structure is not new, and shipping deliberately behind on a framework to dodge a licence
  we accept anyway is a bad trade.
