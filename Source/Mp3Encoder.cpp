#include "Mp3Encoder.h"

#if WAVEFORGE_HAS_MP3
 #include <lame/lame.h>
#endif

namespace wf
{
bool mp3EncodingAvailable()
{
    return WAVEFORGE_HAS_MP3 != 0;
}

#if WAVEFORGE_HAS_MP3
bool writeMp3 (const juce::File& destination, const juce::AudioBuffer<float>& audio, double sampleRate,
               int kbps, juce::String& error)
{
    std::unique_ptr<lame_global_flags, decltype (&lame_close)> lame (lame_init(), &lame_close);

    if (lame == nullptr)
    {
        error = "The MP3 encoder could not start.";
        return false;
    }

    const auto channels = juce::jmin (2, audio.getNumChannels());
    lame_set_in_samplerate (lame.get(), juce::roundToInt (sampleRate));
    lame_set_num_channels (lame.get(), channels);
    lame_set_mode (lame.get(), channels == 1 ? MONO : JOINT_STEREO);
    lame_set_brate (lame.get(), kbps);
    lame_set_quality (lame.get(), 2);   // 0 = best/slowest, 9 = worst; 2 is LAME's "high quality"

    if (lame_init_params (lame.get()) < 0)
    {
        error = "The MP3 encoder does not support " + juce::String (juce::roundToInt (sampleRate)) + " Hz at "
              + juce::String (kbps) + " kbps.";
        return false;
    }

    juce::TemporaryFile temp (destination);
    std::unique_ptr<juce::FileOutputStream> out (temp.getFile().createOutputStream());

    if (out == nullptr || out->failedToOpen())
    {
        error = "Can't write to \"" + destination.getFullPathName() + "\".";
        return false;
    }

    constexpr int blockSize = 8192;
    constexpr int mp3Size = blockSize * 5 / 4 + 7200;   // LAME's recommended worst case
    juce::HeapBlock<unsigned char> mp3 ((size_t) mp3Size);

    const auto* left = audio.getReadPointer (0);
    const auto* right = audio.getReadPointer (channels > 1 ? 1 : 0);

    for (int start = 0; start < audio.getNumSamples(); start += blockSize)
    {
        const auto count = juce::jmin (blockSize, audio.getNumSamples() - start);
        const auto bytes = lame_encode_buffer_ieee_float (lame.get(), left + start, right + start, count, mp3, mp3Size);

        if (bytes < 0 || ! out->write (mp3, (size_t) bytes))
        {
            error = "MP3 encoding failed.";
            return false;
        }
    }

    const auto tail = lame_encode_flush (lame.get(), mp3, mp3Size);

    if (tail < 0 || ! out->write (mp3, (size_t) tail))
    {
        error = "MP3 encoding failed.";
        return false;
    }

    // LAME reserved the first frame for its "Info" tag (exact length and encoder
    // delay, so players skip the padding); fill it in now that encoding is done.
    unsigned char tag[2880];
    const auto tagSize = lame_get_lametag_frame (lame.get(), tag, sizeof (tag));

    if (tagSize > 0 && tagSize <= sizeof (tag) && (! out->setPosition (0) || ! out->write (tag, tagSize)))
    {
        error = "MP3 encoding failed.";
        return false;
    }

    out->flush();
    out.reset();

    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "Can't replace \"" + destination.getFullPathName() + "\".";
        return false;
    }

    return true;
}
#else
bool writeMp3 (const juce::File&, const juce::AudioBuffer<float>&, double, int, juce::String& error)
{
    error = "This build of WaveForge has no MP3 encoder.";
    return false;
}
#endif
} // namespace wf
