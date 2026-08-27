#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace ppm::wire
{

/** Magic bytes 'P','P','M','X' at the head of every audio packet.

    A receiver drops anything that does not start with this, which makes it impossible to
    confuse an audio packet with a stray control frame or with another protocol that
    happened to find the port.
*/
inline constexpr uint8_t magic[4] = { 'P', 'P', 'M', 'X' };

/** Protocol version. A receiver must refuse a version it does not know and say so. */
inline constexpr uint8_t protocolVersion = 1;

/** Header size in bytes.

    32 rather than the 30 the fields need, because the payload offset has to be a multiple
    of 4: the receiver builds a `Float32Array` view directly over the packet's ArrayBuffer
    for the float32 format, and that requires a 4-byte-aligned byteOffset.
*/
inline constexpr int headerBytes = 32;

enum class Format : uint8_t
{
    pcm16 = 0,   ///< signed 16-bit little-endian
    pcm24 = 1,   ///< signed 24-bit little-endian, 3 bytes per sample
    float32 = 2  ///< IEEE 754 single precision little-endian
};

enum Flags : uint8_t
{
    none          = 0,
    discontinuity = 1u << 0,  ///< the sample clock restarted; the receiver must flush
    silence       = 1u << 1,  ///< synthesised silence because the host is not playing
    configChanged = 1u << 2   ///< first packet of a new configEpoch
};

/** Bytes occupied by one sample in the given format. */
constexpr int bytesPerSample (Format format)
{
    switch (format)
    {
        case Format::pcm16:   return 2;
        case Format::pcm24:   return 3;
        case Format::float32: return 4;
    }

    return 4;
}

const char* formatName (Format format);

/** Everything that describes how to interpret a packet's payload.

    Carried in full in *every* packet, not just in the handshake, because receivers join
    mid-stream, control frames get lost, and hosts change sample rate underneath a running
    plugin. A receiver that sees only one packet still knows exactly what it is holding.
*/
struct PacketInfo
{
    Format format = Format::pcm16;
    uint8_t channels = 2;
    uint8_t flags = Flags::none;
    uint32_t sequence = 0;
    uint32_t sampleRate = 48000;
    uint16_t frames = 512;
    uint16_t configEpoch = 0;
    uint64_t sampleClock = 0;   ///< absolute frame index of this packet's first frame
    uint32_t senderTimeMs = 0;  ///< low 32 bits of the sender's ms counter; diagnostics only
};

/** Total packet size for the given description. */
constexpr int packetSize (const PacketInfo& info)
{
    return headerBytes + static_cast<int> (info.frames) * static_cast<int> (info.channels)
                             * bytesPerSample (info.format);
}

/** Writes the 32-byte header at the start of `destination`, little-endian throughout.

    `destination` must have room for at least `headerBytes`.
*/
void writeHeader (uint8_t* destination, const PacketInfo& info);

/** Reads a header. Returns false if the magic or version does not match. */
bool readHeader (const uint8_t* source, size_t sourceSize, PacketInfo& info);

/** Converts interleaved float samples into the wire format, writing to `destination`.

    Floats outside [-1, 1] are clipped for the integer formats, because wrapping a hot
    master bus into a full-scale inversion is the single most alarming artefact a
    monitoring tool could produce. float32 is passed through untouched so a receiver can
    still see exactly what the DAW sent, overs included.
*/
void writeSamples (uint8_t* destination, const float* interleaved, int numSamples, Format format);

} // namespace ppm::wire
