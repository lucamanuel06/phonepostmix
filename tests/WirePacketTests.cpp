#include "TestSupport.h"

#include "core/WirePacket.h"

#include <cmath>

PPM_TEST (headerLayoutIsByteForByteStable)
{
    // The receiver reads these offsets with a DataView written by hand in JavaScript, so
    // the layout is a published contract, not an implementation detail. If this test
    // needs changing, the protocol version has to change with it.
    ppm::wire::PacketInfo info;
    info.format       = ppm::wire::Format::pcm24;
    info.channels     = 2;
    info.flags        = ppm::wire::Flags::discontinuity | ppm::wire::Flags::configChanged;
    info.sequence     = 0x01020304;
    info.sampleRate   = 48000;
    info.frames       = 512;
    info.configEpoch  = 7;
    info.sampleClock  = 0x1122334455667788ull;
    info.senderTimeMs = 0xAABBCCDD;

    uint8_t header[ppm::wire::headerBytes] {};
    ppm::wire::writeHeader (header, info);

    PPM_CHECK_EQ (int (header[0]), int ('P'));
    PPM_CHECK_EQ (int (header[1]), int ('P'));
    PPM_CHECK_EQ (int (header[2]), int ('M'));
    PPM_CHECK_EQ (int (header[3]), int ('X'));
    PPM_CHECK_EQ (int (header[4]), 1);          // version
    PPM_CHECK_EQ (int (header[5]), 1);          // pcm24
    PPM_CHECK_EQ (int (header[6]), 2);          // channels
    PPM_CHECK_EQ (int (header[7]), 0x05);       // discontinuity | configChanged

    PPM_CHECK_EQ (int (header[8]),  0x04);      // sequence, little-endian
    PPM_CHECK_EQ (int (header[11]), 0x01);
    PPM_CHECK_EQ (int (header[12]), 48000 & 0xff);
    PPM_CHECK_EQ (int (header[16]), 512 & 0xff);
    PPM_CHECK_EQ (int (header[17]), (512 >> 8) & 0xff);
    PPM_CHECK_EQ (int (header[18]), 7);
    PPM_CHECK_EQ (int (header[20]), 0x88);      // sampleClock, little-endian
    PPM_CHECK_EQ (int (header[27]), 0x11);
    PPM_CHECK_EQ (int (header[28]), 0xDD);
}

PPM_TEST (headerRoundTrips)
{
    ppm::wire::PacketInfo written;
    written.format       = ppm::wire::Format::float32;
    written.channels     = 1;
    written.flags        = ppm::wire::Flags::silence;
    written.sequence     = 4294967295u;
    written.sampleRate   = 96000;
    written.frames       = 1024;
    written.configEpoch  = 65535;
    written.sampleClock  = 18446744073709551615ull;
    written.senderTimeMs = 123456789;

    uint8_t header[ppm::wire::headerBytes] {};
    ppm::wire::writeHeader (header, written);

    ppm::wire::PacketInfo read;
    PPM_CHECK (ppm::wire::readHeader (header, sizeof (header), read));

    PPM_CHECK (read.format == written.format);
    PPM_CHECK_EQ (int (read.channels), int (written.channels));
    PPM_CHECK_EQ (int (read.flags), int (written.flags));
    PPM_CHECK_EQ (read.sequence, written.sequence);
    PPM_CHECK_EQ (read.sampleRate, written.sampleRate);
    PPM_CHECK_EQ (int (read.frames), int (written.frames));
    PPM_CHECK_EQ (int (read.configEpoch), int (written.configEpoch));
    PPM_CHECK (read.sampleClock == written.sampleClock);
    PPM_CHECK_EQ (read.senderTimeMs, written.senderTimeMs);
}

PPM_TEST (headerReadRejectsGarbage)
{
    uint8_t header[ppm::wire::headerBytes] {};
    ppm::wire::PacketInfo info;

    PPM_CHECK (! ppm::wire::readHeader (header, sizeof (header), info));   // no magic

    ppm::wire::writeHeader (header, info);
    PPM_CHECK (ppm::wire::readHeader (header, sizeof (header), info));

    header[4] = 99;   // a version from the future
    PPM_CHECK (! ppm::wire::readHeader (header, sizeof (header), info));

    ppm::wire::writeHeader (header, info);
    PPM_CHECK (! ppm::wire::readHeader (header, 10, info));                // truncated
}

PPM_TEST (payloadOffsetIsFourByteAlignedForFloatViews)
{
    // The receiver constructs `new Float32Array(buffer, headerBytes, n)`, which throws a
    // RangeError unless the offset is a multiple of 4.
    PPM_CHECK_EQ (ppm::wire::headerBytes % 4, 0);
}

PPM_TEST (packetSizeMatchesTheFormats)
{
    ppm::wire::PacketInfo info;
    info.channels = 2;
    info.frames = 512;

    info.format = ppm::wire::Format::pcm16;
    PPM_CHECK_EQ (ppm::wire::packetSize (info), 32 + 512 * 2 * 2);

    info.format = ppm::wire::Format::pcm24;
    PPM_CHECK_EQ (ppm::wire::packetSize (info), 32 + 512 * 2 * 3);

    info.format = ppm::wire::Format::float32;
    PPM_CHECK_EQ (ppm::wire::packetSize (info), 32 + 512 * 2 * 4);
}

PPM_TEST (pcm16ConversionIsCorrectAndDoesNotWrapAtFullScale)
{
    const float input[] = { 0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 0.5f };
    uint8_t out[sizeof (input) / sizeof (float) * 2] {};

    ppm::wire::writeSamples (out, input, 6, ppm::wire::Format::pcm16);

    auto sampleAt = [&out] (int i)
    {
        return static_cast<int16_t> (static_cast<uint16_t> (out[i * 2]) | (static_cast<uint16_t> (out[i * 2 + 1]) << 8));
    };

    PPM_CHECK_EQ (int (sampleAt (0)), 0);
    PPM_CHECK_EQ (int (sampleAt (1)), 32767);
    PPM_CHECK_EQ (int (sampleAt (2)), -32767);

    // The whole point: an over must clip to full scale, never wrap to the opposite rail.
    PPM_CHECK_EQ (int (sampleAt (3)), 32767);
    PPM_CHECK_EQ (int (sampleAt (4)), -32767);
    PPM_CHECK_EQ (int (sampleAt (5)), 16384);
}

PPM_TEST (pcm24ConversionUsesThreeLittleEndianBytes)
{
    const float input[] = { 1.0f, -1.0f, 0.0f };
    uint8_t out[9] {};

    ppm::wire::writeSamples (out, input, 3, ppm::wire::Format::pcm24);

    auto sampleAt = [&out] (int i)
    {
        const auto bits = static_cast<uint32_t> (out[i * 3])
                        | (static_cast<uint32_t> (out[i * 3 + 1]) << 8)
                        | (static_cast<uint32_t> (out[i * 3 + 2]) << 16);

        // Sign-extend from 24 to 32 bits.
        return static_cast<int32_t> (bits & 0x800000 ? bits | 0xff000000u : bits);
    };

    PPM_CHECK_EQ (sampleAt (0), 8388607);
    PPM_CHECK_EQ (sampleAt (1), -8388607);
    PPM_CHECK_EQ (sampleAt (2), 0);
}

PPM_TEST (float32ConversionPreservesBitsIncludingOvers)
{
    // float32 is the transparent path: a receiver must be able to see exactly what the
    // master bus contained, overs and all, because clipping here would hide the very
    // thing an engineer is checking for.
    const float input[] = { 0.0f, 1.5f, -1.5f, 0.123456789f };
    uint8_t out[16] {};

    ppm::wire::writeSamples (out, input, 4, ppm::wire::Format::float32);

    for (int i = 0; i < 4; ++i)
    {
        uint32_t bits = static_cast<uint32_t> (out[i * 4])
                      | (static_cast<uint32_t> (out[i * 4 + 1]) << 8)
                      | (static_cast<uint32_t> (out[i * 4 + 2]) << 16)
                      | (static_cast<uint32_t> (out[i * 4 + 3]) << 24);

        float decoded;
        std::memcpy (&decoded, &bits, sizeof (decoded));
        PPM_CHECK (decoded == input[i]);
    }
}
