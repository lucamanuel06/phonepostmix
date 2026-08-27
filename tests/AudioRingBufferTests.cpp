#include "TestSupport.h"

#include "core/AudioRingBuffer.h"

#include <thread>

namespace
{

juce::AudioBuffer<float> makeRamp (int numChannels, int numFrames, float start)
{
    juce::AudioBuffer<float> b (numChannels, numFrames);

    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numFrames; ++i)
            b.setSample (ch, i, start + static_cast<float> (i) + static_cast<float> (ch) * 1000.0f);

    return b;
}

} // namespace

PPM_TEST (ringBufferRoundTripsInterleavedSamples)
{
    ppm::AudioRingBuffer ring;
    ring.prepare (2, 1024);

    const auto source = makeRamp (2, 8, 0.0f);
    PPM_CHECK (ring.write (source));
    PPM_CHECK_EQ (ring.getNumReady(), 8);

    std::vector<float> out (16, -1.0f);
    PPM_CHECK_EQ (ring.read (out.data(), 8), 8);

    for (int i = 0; i < 8; ++i)
    {
        PPM_CHECK_EQ (out[static_cast<size_t> (i) * 2 + 0], static_cast<float> (i));
        PPM_CHECK_EQ (out[static_cast<size_t> (i) * 2 + 1], static_cast<float> (i) + 1000.0f);
    }

    PPM_CHECK_EQ (ring.getNumReady(), 0);
}

PPM_TEST (ringBufferWrapsAroundTheEndOfStorage)
{
    ppm::AudioRingBuffer ring;
    ring.prepare (2, 16);

    std::vector<float> out (64, 0.0f);

    // Push and drain repeatedly so the read and write cursors walk past the end of the
    // storage several times; this is where an off-by-one in the two-region copy shows up.
    for (int round = 0; round < 20; ++round)
    {
        const auto source = makeRamp (2, 6, static_cast<float> (round * 6));
        PPM_CHECK (ring.write (source));
        PPM_CHECK_EQ (ring.read (out.data(), 6), 6);

        for (int i = 0; i < 6; ++i)
            PPM_CHECK_EQ (out[static_cast<size_t> (i) * 2], static_cast<float> (round * 6 + i));
    }
}

PPM_TEST (ringBufferDiscardsWholeBlocksOnOverrun)
{
    ppm::AudioRingBuffer ring;
    ring.prepare (2, 16);

    PPM_CHECK (ring.write (makeRamp (2, 10, 0.0f)));
    PPM_CHECK_EQ (ring.getOverrunCount(), 0);

    // Only 5 frames of space remain (the FIFO keeps one frame of headroom), so a 10-frame
    // block must be rejected outright rather than partially written.
    PPM_CHECK (! ring.write (makeRamp (2, 10, 100.0f)));
    PPM_CHECK_EQ (ring.getOverrunCount(), 1);
    PPM_CHECK_EQ (ring.getNumReady(), 10);
}

PPM_TEST (ringBufferUpmixesMonoSourceToStereo)
{
    ppm::AudioRingBuffer ring;
    ring.prepare (2, 64);

    PPM_CHECK (ring.write (makeRamp (1, 4, 0.0f)));

    std::vector<float> out (8, -1.0f);
    PPM_CHECK_EQ (ring.read (out.data(), 4), 4);

    for (int i = 0; i < 4; ++i)
        PPM_CHECK_EQ (out[static_cast<size_t> (i) * 2 + 0], out[static_cast<size_t> (i) * 2 + 1]);
}

PPM_TEST (ringBufferSurvivesConcurrentProducerAndConsumer)
{
    ppm::AudioRingBuffer ring;
    ring.prepare (2, 4096);

    constexpr int blockSize = 128;
    constexpr int totalBlocks = 4000;

    std::atomic<bool> producerDone { false };
    std::atomic<long long> framesRead { 0 };
    std::atomic<bool> sawTornFrame { false };

    std::thread consumer ([&]
    {
        std::vector<float> out (static_cast<size_t> (blockSize) * 2);

        while (! producerDone.load() || ring.getNumReady() > 0)
        {
            const auto n = ring.read (out.data(), blockSize);

            // Every frame is written with both channels equal, so a torn frame — one
            // where the reader saw channel 0 of one block and channel 1 of the next —
            // is detectable without any synchronisation.
            for (int i = 0; i < n; ++i)
                if (out[static_cast<size_t> (i) * 2] != out[static_cast<size_t> (i) * 2 + 1])
                    sawTornFrame = true;

            framesRead += n;
        }
    });

    juce::AudioBuffer<float> block (2, blockSize);

    for (int b = 0; b < totalBlocks; ++b)
    {
        const auto value = static_cast<float> (b);
        block.clear();

        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (block.getWritePointer (ch), value, blockSize);

        while (! ring.write (block))
            std::this_thread::yield();
    }

    producerDone = true;
    consumer.join();

    PPM_CHECK (! sawTornFrame.load());
    PPM_CHECK_EQ (framesRead.load(), static_cast<long long> (totalBlocks) * blockSize);
}
