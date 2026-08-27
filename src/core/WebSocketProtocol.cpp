#include "core/WebSocketProtocol.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstring>

namespace ppm::websocket
{

std::string makeAcceptKey (const std::string& clientKey)
{
    const auto digest = Sha1::hash (clientKey + handshakeGuid);

    juce::MemoryOutputStream encoded;
    juce::Base64::convertToBase64 (encoded, digest.data(), digest.size());

    return encoded.toString().toStdString();
}

void writeFrame (std::vector<uint8_t>& out, Opcode opcode, const void* payload, size_t payloadSize)
{
    out.push_back (static_cast<uint8_t> (0x80 | static_cast<uint8_t> (opcode))); // FIN + opcode

    if (payloadSize < 126)
    {
        out.push_back (static_cast<uint8_t> (payloadSize));
    }
    else if (payloadSize <= 0xffff)
    {
        out.push_back (126);
        out.push_back (static_cast<uint8_t> ((payloadSize >> 8) & 0xff));
        out.push_back (static_cast<uint8_t> (payloadSize & 0xff));
    }
    else
    {
        out.push_back (127);
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back (static_cast<uint8_t> ((static_cast<uint64_t> (payloadSize) >> shift) & 0xff));
    }

    const auto* bytes = static_cast<const uint8_t*> (payload);
    out.insert (out.end(), bytes, bytes + payloadSize);
}

void writeTextFrame (std::vector<uint8_t>& out, const std::string& text)
{
    writeFrame (out, Opcode::text, text.data(), text.size());
}

void FrameParser::push (const uint8_t* data, size_t numBytes)
{
    if (errored)
        return;

    // Drop the already-parsed prefix before growing, so a long-lived connection does not
    // accumulate an unbounded buffer of consumed bytes.
    if (consumed > 0 && consumed == incoming.size())
    {
        incoming.clear();
        consumed = 0;
    }
    else if (consumed > 4096)
    {
        incoming.erase (incoming.begin(), incoming.begin() + static_cast<long> (consumed));
        consumed = 0;
    }

    incoming.insert (incoming.end(), data, data + numBytes);
}

void FrameParser::fail (std::string message)
{
    errored = true;
    errorMessage = std::move (message);
}

std::optional<Frame> FrameParser::nextFrame()
{
    while (! errored)
    {
        const auto available = incoming.size() - consumed;

        if (available < 2)
            return std::nullopt;

        const auto* p = incoming.data() + consumed;

        const bool fin = (p[0] & 0x80) != 0;
        const auto opcode = static_cast<Opcode> (p[0] & 0x0f);
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t payloadSize = p[1] & 0x7f;

        size_t headerSize = 2;

        if (payloadSize == 126)
        {
            if (available < headerSize + 2)
                return std::nullopt;

            payloadSize = (uint64_t (p[2]) << 8) | uint64_t (p[3]);
            headerSize += 2;
        }
        else if (payloadSize == 127)
        {
            if (available < headerSize + 8)
                return std::nullopt;

            payloadSize = 0;
            for (int i = 0; i < 8; ++i)
                payloadSize = (payloadSize << 8) | uint64_t (p[2 + i]);

            headerSize += 8;
        }

        if (! masked)
        {
            // RFC 6455 §5.1: "The server MUST close the connection upon receiving a
            // frame that is not masked."
            fail ("client sent an unmasked frame");
            return std::nullopt;
        }

        if (payloadSize > maxPayloadSize)
        {
            fail ("client frame exceeds the maximum payload size");
            return std::nullopt;
        }

        const size_t maskOffset = headerSize;
        headerSize += 4;

        if (available < headerSize + payloadSize)
            return std::nullopt;

        const auto* mask = p + maskOffset;
        const auto* body = p + headerSize;

        std::vector<uint8_t> payload (static_cast<size_t> (payloadSize));
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = body[i] ^ mask[i % 4];

        consumed += headerSize + static_cast<size_t> (payloadSize);

        // Control frames may be interleaved inside a fragmented message and are never
        // themselves fragmented, so they are returned immediately without touching the
        // reassembly state.
        const bool isControl = (static_cast<uint8_t> (opcode) & 0x08) != 0;

        if (isControl)
            return Frame { opcode, true, std::move (payload) };

        if (opcode == Opcode::continuation)
        {
            if (! assembling)
            {
                fail ("continuation frame with no message in progress");
                return std::nullopt;
            }

            assembly.insert (assembly.end(), payload.begin(), payload.end());

            if (assembly.size() > maxPayloadSize)
            {
                fail ("reassembled message exceeds the maximum payload size");
                return std::nullopt;
            }

            if (! fin)
                continue;

            assembling = false;
            return Frame { assemblyOpcode, true, std::move (assembly) };
        }

        if (fin)
            return Frame { opcode, true, std::move (payload) };

        assembling = true;
        assemblyOpcode = opcode;
        assembly = std::move (payload);
    }

    return std::nullopt;
}

} // namespace ppm::websocket
