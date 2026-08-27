#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace ppm
{

/** A single-producer / single-consumer lock-free ring buffer for interleaved float audio.

    The producer is the audio thread inside `processBlock`; the consumer is the streaming
    thread. `write` never allocates, never locks and never blocks, which is the whole
    reason this class exists — see CONTRIBUTING.md.

    Storage is interleaved because everything downstream of here (packet payloads, the
    browser's Web Audio ring) is interleaved too, so converting once at the producer is
    cheaper than converting on the consumer for every packet.

    Overflow policy: if the consumer falls behind, `write` discards the incoming block
    rather than blocking or overwriting data the consumer is mid-read on, and increments
    `getOverrunCount`. Discarding is correct for live monitoring: a listener wants the
    newest audio to keep flowing, and the receiver detects the gap from the packet
    sequence numbers. Overruns in practice mean the network stalled, not that the buffer
    is too small.
*/
class AudioRingBuffer
{
public:
    AudioRingBuffer() = default;

    /** Allocates storage. Call from `prepareToPlay`, never from the audio thread.

        @param numChannels   channels per frame
        @param numFrames     capacity in frames; rounded up internally to leave the FIFO
                             one frame of headroom
    */
    void prepare (int numChannels, int numFrames)
    {
        jassert (numChannels > 0 && numFrames > 0);

        channels = numChannels;
        storage.assign (static_cast<size_t> (numChannels) * static_cast<size_t> (numFrames), 0.0f);
        fifo.setTotalSize (numFrames);
        reset();
    }

    /** Releases storage. Call from `releaseResources`, never from the audio thread. */
    void release()
    {
        storage.clear();
        storage.shrink_to_fit();
        fifo.setTotalSize (1);
        channels = 0;
    }

    /** Discards all buffered audio. Not safe to call while either thread is running. */
    void reset()
    {
        fifo.reset();
        overruns.store (0, std::memory_order_relaxed);
    }

    int getNumChannels() const noexcept   { return channels; }
    int getCapacity() const noexcept      { return fifo.getTotalSize(); }
    int getNumReady() const noexcept      { return fifo.getNumReady(); }
    int getFreeSpace() const noexcept     { return fifo.getFreeSpace(); }

    /** Number of blocks the producer had to discard because the consumer fell behind. */
    int getOverrunCount() const noexcept  { return overruns.load (std::memory_order_relaxed); }

    /** Copies a JUCE audio buffer in, de-planarising it.

        Real-time safe. Returns false and counts an overrun if the block does not fit; the
        block is then discarded whole, because writing half a block would tear frames.

        Channels beyond `getNumChannels()` are ignored; if the source has fewer channels
        than the ring, the last source channel is duplicated (so a mono master still
        arrives as a valid stereo stream).
    */
    bool write (const juce::AudioBuffer<float>& source) noexcept
    {
        const auto numFrames = source.getNumSamples();

        if (numFrames <= 0)
            return true;

        if (channels <= 0 || fifo.getFreeSpace() < numFrames)
        {
            overruns.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const auto sourceChannels = source.getNumChannels();
        jassert (sourceChannels > 0);

        const auto scope = fifo.write (numFrames);

        auto copyRegion = [&] (int startIndex, int blockSize, int sourceOffset)
        {
            if (blockSize <= 0)
                return;

            auto* dest = storage.data() + static_cast<size_t> (startIndex) * static_cast<size_t> (channels);

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* src = source.getReadPointer (juce::jmin (ch, sourceChannels - 1), sourceOffset);

                for (int i = 0; i < blockSize; ++i)
                    dest[static_cast<size_t> (i) * static_cast<size_t> (channels) + static_cast<size_t> (ch)] = src[i];
            }
        };

        copyRegion (scope.startIndex1, scope.blockSize1, 0);
        copyRegion (scope.startIndex2, scope.blockSize2, scope.blockSize1);

        return true;
    }

    /** Copies up to `maxFrames` frames out into an interleaved destination.

        Called from the streaming thread. `destination` must have room for
        `maxFrames * getNumChannels()` floats. Returns the number of frames written.
    */
    int read (float* destination, int maxFrames) noexcept
    {
        if (channels <= 0 || maxFrames <= 0)
            return 0;

        const auto numFrames = juce::jmin (maxFrames, fifo.getNumReady());

        if (numFrames <= 0)
            return 0;

        const auto scope = fifo.read (numFrames);

        auto copyRegion = [&] (int startIndex, int blockSize, int destOffsetFrames)
        {
            if (blockSize <= 0)
                return;

            const auto floats = static_cast<size_t> (blockSize) * static_cast<size_t> (channels);
            std::memcpy (destination + static_cast<size_t> (destOffsetFrames) * static_cast<size_t> (channels),
                         storage.data() + static_cast<size_t> (startIndex) * static_cast<size_t> (channels),
                         floats * sizeof (float));
        };

        copyRegion (scope.startIndex1, scope.blockSize1, 0);
        copyRegion (scope.startIndex2, scope.blockSize2, scope.blockSize1);

        return numFrames;
    }

private:
    juce::AbstractFifo fifo { 1 };
    std::vector<float> storage;
    int channels = 0;
    std::atomic<int> overruns { 0 };

    JUCE_DECLARE_NON_COPYABLE (AudioRingBuffer)
};

} // namespace ppm
