#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ppm
{

/** A minimal SHA-1 implementation (FIPS 180-4).

    JUCE ships MD5, SHA-256 and Whirlpool but no SHA-1, and the WebSocket opening
    handshake (RFC 6455 §4.2.2) mandates SHA-1 for the `Sec-WebSocket-Accept` value. That
    single use is the only reason this file exists.

    SHA-1 is cryptographically broken and must not be used for anything security-relevant.
    Here it is a fixed protocol constant, not a security primitive: RFC 6455's handshake
    hash exists to prove the peer understood the protocol, not to protect anything.
*/
class Sha1
{
public:
    using Digest = std::array<uint8_t, 20>;

    Sha1() { reset(); }

    void reset()
    {
        state = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
        bitCount = 0;
        bufferUsed = 0;
    }

    void update (const void* data, size_t numBytes)
    {
        const auto* bytes = static_cast<const uint8_t*> (data);
        bitCount += static_cast<uint64_t> (numBytes) * 8;

        while (numBytes > 0)
        {
            const auto take = std::min (numBytes, size_t (64) - bufferUsed);
            std::memcpy (buffer.data() + bufferUsed, bytes, take);
            bufferUsed += take;
            bytes += take;
            numBytes -= take;

            if (bufferUsed == 64)
            {
                processBlock (buffer.data());
                bufferUsed = 0;
            }
        }
    }

    void update (const std::string& s) { update (s.data(), s.size()); }

    /** Finalises and returns the digest. The object is left reset and reusable. */
    Digest finish()
    {
        const auto totalBits = bitCount;

        const uint8_t padStart = 0x80;
        update (&padStart, 1);

        const uint8_t zero = 0;
        while (bufferUsed != 56)
            update (&zero, 1);

        uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = static_cast<uint8_t> ((totalBits >> (56 - i * 8)) & 0xff);

        update (lengthBytes, 8);

        Digest out {};
        for (size_t i = 0; i < 5; ++i)
        {
            out[i * 4 + 0] = static_cast<uint8_t> ((state[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<uint8_t> ((state[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<uint8_t> ((state[i] >>  8) & 0xff);
            out[i * 4 + 3] = static_cast<uint8_t> ( state[i]        & 0xff);
        }

        reset();
        return out;
    }

    static Digest hash (const std::string& s)
    {
        Sha1 h;
        h.update (s);
        return h.finish();
    }

private:
    static uint32_t rotl (uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void processBlock (const uint8_t* block)
    {
        uint32_t w[80];

        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t (block[i * 4]) << 24) | (uint32_t (block[i * 4 + 1]) << 16)
                 | (uint32_t (block[i * 4 + 2]) << 8) | uint32_t (block[i * 4 + 3]);

        for (int i = 16; i < 80; ++i)
            w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        auto a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;

            if (i < 20)       { f = (b & c) | (~b & d);            k = 0x5A827999u; }
            else if (i < 40)  { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60)  { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else              { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }

            const auto temp = rotl (a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl (b, 30);
            b = a;
            a = temp;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    std::array<uint32_t, 5> state {};
    std::array<uint8_t, 64> buffer {};
    size_t bufferUsed = 0;
    uint64_t bitCount = 0;
};

} // namespace ppm
