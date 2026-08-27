#include "core/WirePacket.h"

#include <algorithm>
#include <cmath>

namespace ppm::wire
{

namespace
{

template <typename T>
void writeLittleEndian (uint8_t* destination, T value)
{
    for (size_t i = 0; i < sizeof (T); ++i)
        destination[i] = static_cast<uint8_t> ((value >> (i * 8)) & 0xff);
}

template <typename T>
T readLittleEndian (const uint8_t* source)
{
    T value = 0;

    for (size_t i = 0; i < sizeof (T); ++i)
        value |= static_cast<T> (source[i]) << (i * 8);

    return value;
}

} // namespace

const char* formatName (Format format)
{
    switch (format)
    {
        case Format::pcm16:   return "pcm16";
        case Format::pcm24:   return "pcm24";
        case Format::float32: return "float32";
    }

    return "unknown";
}

void writeHeader (uint8_t* destination, const PacketInfo& info)
{
    std::memcpy (destination, magic, 4);

    destination[4] = protocolVersion;
    destination[5] = static_cast<uint8_t> (info.format);
    destination[6] = info.channels;
    destination[7] = info.flags;

    writeLittleEndian<uint32_t> (destination + 8,  info.sequence);
    writeLittleEndian<uint32_t> (destination + 12, info.sampleRate);
    writeLittleEndian<uint16_t> (destination + 16, info.frames);
    writeLittleEndian<uint16_t> (destination + 18, info.configEpoch);
    writeLittleEndian<uint64_t> (destination + 20, info.sampleClock);
    writeLittleEndian<uint32_t> (destination + 28, info.senderTimeMs);
}

bool readHeader (const uint8_t* source, size_t sourceSize, PacketInfo& info)
{
    if (sourceSize < static_cast<size_t> (headerBytes))
        return false;

    if (std::memcmp (source, magic, 4) != 0)
        return false;

    if (source[4] != protocolVersion)
        return false;

    info.format       = static_cast<Format> (source[5]);
    info.channels     = source[6];
    info.flags        = source[7];
    info.sequence     = readLittleEndian<uint32_t> (source + 8);
    info.sampleRate   = readLittleEndian<uint32_t> (source + 12);
    info.frames       = readLittleEndian<uint16_t> (source + 16);
    info.configEpoch  = readLittleEndian<uint16_t> (source + 18);
    info.sampleClock  = readLittleEndian<uint64_t> (source + 20);
    info.senderTimeMs = readLittleEndian<uint32_t> (source + 28);

    return true;
}

void writeSamples (uint8_t* destination, const float* interleaved, int numSamples, Format format)
{
    switch (format)
    {
        case Format::pcm16:
            for (int i = 0; i < numSamples; ++i)
            {
                const auto clamped = std::clamp (interleaved[i], -1.0f, 1.0f);

                // 32767 rather than 32768: scaling by 32768 makes -1.0 map to -32768 but
                // +1.0 overflow to -32768 as well, which is the classic full-scale
                // inversion click.
                const auto value = static_cast<int16_t> (std::lround (clamped * 32767.0f));
                writeLittleEndian<uint16_t> (destination + i * 2, static_cast<uint16_t> (value));
            }
            break;

        case Format::pcm24:
            for (int i = 0; i < numSamples; ++i)
            {
                const auto clamped = std::clamp (interleaved[i], -1.0f, 1.0f);
                const auto value = static_cast<int32_t> (std::lround (clamped * 8388607.0));
                const auto bits = static_cast<uint32_t> (value);

                destination[i * 3 + 0] = static_cast<uint8_t> (bits & 0xff);
                destination[i * 3 + 1] = static_cast<uint8_t> ((bits >> 8) & 0xff);
                destination[i * 3 + 2] = static_cast<uint8_t> ((bits >> 16) & 0xff);
            }
            break;

        case Format::float32:
            for (int i = 0; i < numSamples; ++i)
            {
                uint32_t bits;
                std::memcpy (&bits, &interleaved[i], sizeof (bits));
                writeLittleEndian<uint32_t> (destination + i * 4, bits);
            }
            break;
    }
}

} // namespace ppm::wire
