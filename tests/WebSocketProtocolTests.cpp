#include "TestSupport.h"

#include "core/WebSocketProtocol.h"

#include <iomanip>
#include <sstream>

namespace
{

std::string toHex (const ppm::Sha1::Digest& d)
{
    std::ostringstream s;
    s << std::hex << std::setfill ('0');

    for (auto b : d)
        s << std::setw (2) << static_cast<int> (b);

    return s.str();
}

/** Builds a client-to-server frame: masked, as RFC 6455 requires of clients. */
std::vector<uint8_t> makeClientFrame (ppm::websocket::Opcode opcode,
                                      const std::vector<uint8_t>& payload,
                                      bool fin = true,
                                      std::array<uint8_t, 4> mask = { 0x37, 0xfa, 0x21, 0x3d })
{
    std::vector<uint8_t> f;
    f.push_back (static_cast<uint8_t> ((fin ? 0x80 : 0x00) | static_cast<uint8_t> (opcode)));

    const auto size = payload.size();

    if (size < 126)
    {
        f.push_back (static_cast<uint8_t> (0x80 | size));
    }
    else if (size <= 0xffff)
    {
        f.push_back (0x80 | 126);
        f.push_back (static_cast<uint8_t> ((size >> 8) & 0xff));
        f.push_back (static_cast<uint8_t> (size & 0xff));
    }
    else
    {
        f.push_back (0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            f.push_back (static_cast<uint8_t> ((static_cast<uint64_t> (size) >> shift) & 0xff));
    }

    f.insert (f.end(), mask.begin(), mask.end());

    for (size_t i = 0; i < size; ++i)
        f.push_back (payload[i] ^ mask[i % 4]);

    return f;
}

std::vector<uint8_t> bytesOf (const std::string& s)
{
    return { s.begin(), s.end() };
}

} // namespace

PPM_TEST (sha1MatchesFips180TestVectors)
{
    PPM_CHECK (toHex (ppm::Sha1::hash ("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    PPM_CHECK (toHex (ppm::Sha1::hash ("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    PPM_CHECK (toHex (ppm::Sha1::hash ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
               == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // Multi-block input with a length that forces an extra padding block.
    PPM_CHECK (toHex (ppm::Sha1::hash (std::string (1000000, 'a')))
               == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

PPM_TEST (acceptKeyMatchesTheRfc6455Example)
{
    // RFC 6455 §1.3 worked example.
    PPM_CHECK (ppm::websocket::makeAcceptKey ("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

PPM_TEST (writeFrameUsesTheShortestLengthEncoding)
{
    using namespace ppm::websocket;

    std::vector<uint8_t> out;
    writeFrame (out, Opcode::binary, "hi", 2);
    PPM_CHECK_EQ (out.size(), size_t (4));
    PPM_CHECK_EQ (int (out[0]), 0x82);   // FIN + binary
    PPM_CHECK_EQ (int (out[1]), 2);      // unmasked, 2-byte payload

    out.clear();
    const std::vector<uint8_t> medium (200, 0xab);
    writeFrame (out, Opcode::binary, medium.data(), medium.size());
    PPM_CHECK_EQ (int (out[1]), 126);
    PPM_CHECK_EQ (out.size(), medium.size() + 4);

    out.clear();
    const std::vector<uint8_t> large (70000, 0xcd);
    writeFrame (out, Opcode::binary, large.data(), large.size());
    PPM_CHECK_EQ (int (out[1]), 127);
    PPM_CHECK_EQ (out.size(), large.size() + 10);
}

PPM_TEST (parserUnmasksASingleFrame)
{
    using namespace ppm::websocket;

    FrameParser parser;
    const auto frame = makeClientFrame (Opcode::text, bytesOf ("Hello"));
    parser.push (frame.data(), frame.size());

    const auto parsed = parser.nextFrame();
    PPM_CHECK (parsed.has_value());
    PPM_CHECK (parsed && parsed->opcode == Opcode::text);
    PPM_CHECK (parsed && std::string (parsed->payload.begin(), parsed->payload.end()) == "Hello");
    PPM_CHECK (! parser.nextFrame().has_value());
    PPM_CHECK (! parser.hasError());
}

PPM_TEST (parserReassemblesAcrossArbitraryByteSplits)
{
    using namespace ppm::websocket;

    FrameParser parser;
    const std::string message (5000, 'x');
    const auto frame = makeClientFrame (Opcode::binary, bytesOf (message));

    // Feed one byte at a time: the parser must survive every possible split point,
    // which is exactly what a real socket will do to it.
    std::optional<Frame> parsed;

    for (size_t i = 0; i < frame.size(); ++i)
    {
        parser.push (frame.data() + i, 1);
        if (auto f = parser.nextFrame())
            parsed = std::move (f);
    }

    PPM_CHECK (parsed.has_value());
    PPM_CHECK (parsed && parsed->payload.size() == message.size());
    PPM_CHECK (! parser.hasError());
}

PPM_TEST (parserJoinsFragmentedMessages)
{
    using namespace ppm::websocket;

    FrameParser parser;

    auto first = makeClientFrame (Opcode::text, bytesOf ("Hel"), false);
    auto rest  = makeClientFrame (Opcode::continuation, bytesOf ("lo"), true);

    parser.push (first.data(), first.size());
    PPM_CHECK (! parser.nextFrame().has_value());

    parser.push (rest.data(), rest.size());
    const auto parsed = parser.nextFrame();

    PPM_CHECK (parsed.has_value());
    PPM_CHECK (parsed && parsed->opcode == Opcode::text);
    PPM_CHECK (parsed && std::string (parsed->payload.begin(), parsed->payload.end()) == "Hello");
}

PPM_TEST (parserReturnsControlFramesDuringReassembly)
{
    using namespace ppm::websocket;

    FrameParser parser;

    auto first = makeClientFrame (Opcode::text, bytesOf ("part"), false);
    auto ping  = makeClientFrame (Opcode::ping, bytesOf ("p"));
    auto rest  = makeClientFrame (Opcode::continuation, bytesOf ("ial"), true);

    parser.push (first.data(), first.size());
    parser.push (ping.data(), ping.size());
    parser.push (rest.data(), rest.size());

    const auto a = parser.nextFrame();
    PPM_CHECK (a && a->opcode == Opcode::ping);

    const auto b = parser.nextFrame();
    PPM_CHECK (b && b->opcode == Opcode::text);
    PPM_CHECK (b && std::string (b->payload.begin(), b->payload.end()) == "partial");
}

PPM_TEST (parserRejectsUnmaskedClientFrames)
{
    using namespace ppm::websocket;

    FrameParser parser;

    // A server-style (unmasked) frame arriving from a client is a protocol violation.
    std::vector<uint8_t> unmasked;
    writeFrame (unmasked, Opcode::text, "nope", 4);
    parser.push (unmasked.data(), unmasked.size());

    PPM_CHECK (! parser.nextFrame().has_value());
    PPM_CHECK (parser.hasError());
}

PPM_TEST (parserRejectsOversizedFrames)
{
    using namespace ppm::websocket;

    FrameParser parser { 64 };
    const auto frame = makeClientFrame (Opcode::binary, std::vector<uint8_t> (200, 0x01));
    parser.push (frame.data(), frame.size());

    PPM_CHECK (! parser.nextFrame().has_value());
    PPM_CHECK (parser.hasError());
}
