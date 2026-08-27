#pragma once

#include "core/Sha1.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace ppm::websocket
{

/** Frame opcodes from RFC 6455 §5.2. */
enum class Opcode : uint8_t
{
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xA
};

struct Frame
{
    Opcode opcode = Opcode::binary;
    bool fin = true;
    std::vector<uint8_t> payload;
};

/** The GUID every WebSocket handshake concatenates with the client key (RFC 6455 §1.3). */
inline constexpr const char* handshakeGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/** Computes the `Sec-WebSocket-Accept` header value for a client's `Sec-WebSocket-Key`.

    Base64(SHA1(key + GUID)). Implemented here rather than pulled from a library because
    it is four lines and the alternative is a dependency.
*/
std::string makeAcceptKey (const std::string& clientKey);

/** Appends a complete, unfragmented, unmasked frame to `out`.

    Server-to-client frames must never be masked (RFC 6455 §5.1), which is why this
    writer has no masking path at all.
*/
void writeFrame (std::vector<uint8_t>& out, Opcode opcode, const void* payload, size_t payloadSize);

/** Convenience overload for a text frame. */
void writeTextFrame (std::vector<uint8_t>& out, const std::string& text);

/** Incremental parser for client-to-server frames.

    Feed it whatever bytes arrive from the socket, in whatever sizes they arrive; pull
    complete frames out with `nextFrame`. Continuation frames are reassembled, so callers
    only ever see whole messages.

    The parser enforces the two rules a server must enforce: client frames are always
    masked, and a payload may not exceed `maxPayloadSize`. Violating either sets the error
    flag, after which the connection should be closed — a malformed or oversized frame is
    not recoverable, and a browser will never legitimately send one.
*/
class FrameParser
{
public:
    explicit FrameParser (size_t maxPayloadSizeIn = 1u << 20) : maxPayloadSize (maxPayloadSizeIn) {}

    void push (const uint8_t* data, size_t numBytes);

    /** Returns the next complete message, or nullopt if more bytes are needed. */
    std::optional<Frame> nextFrame();

    bool hasError() const noexcept              { return errored; }
    const std::string& getError() const noexcept { return errorMessage; }

private:
    void fail (std::string message);

    std::vector<uint8_t> incoming;
    size_t consumed = 0;
    size_t maxPayloadSize;

    // Reassembly state for fragmented messages.
    bool assembling = false;
    Opcode assemblyOpcode = Opcode::binary;
    std::vector<uint8_t> assembly;

    bool errored = false;
    std::string errorMessage;
};

} // namespace ppm::websocket
