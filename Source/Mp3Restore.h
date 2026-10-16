#pragma once

#include "Effects.h"

// Tools > MP3 Restore: undoes some of what lossy encoding (MP3, low-bitrate
// AAC) does to music:
//  - the top end it cut off (everything above ~16 kHz at 128 kbps) is grown
//    back from the band just below the cut, by saturation (harmonics) or by
//    copying that band's spectrum up, following its loudness from moment to
//    moment (like the SBR of newer codecs);
//  - the "swirl" / "birdies" (bands of the spectrum switching on and off) are
//    smoothed out in time and frequency, and holes are filled;
//  - optional warmth (tape-like saturation) and punch (sharper attacks, against
//    the smeared transients and pre-echo).
namespace wf::fx
{
struct Mp3RestoreParams
{
    double cutoffHz = 16000.0;   // where the encoder cut the top end
    double highs = 0.6;          // 0..1, how much top end to grow back
    bool spectralCopy = false;   // false = harmonics (saturation), true = copy the spectrum up
    double deswirl = 0.4;        // 0..1
    double warmth = 0.15;        // 0..1
    double punch = 0.0;          // 0..1
    double outputDb = 0.0;
};

std::shared_ptr<Processor> makeMp3Restore();
void setParams (Processor&, const Mp3RestoreParams&);

// The frequency above which (almost) nothing is left, looking at the average
// spectrum; empty when there's no clear, steep cut below ~95 % of Nyquist.
// `spectrumDb` gets the average spectrum (one value per bin of a 4096 FFT).
std::optional<double> detectCutoff (const juce::AudioBuffer<float>& audio, double rate, std::vector<float>* spectrumDb = nullptr);

// "about 128 kbps" for a cutoff, from what LAME usually does.
juce::String likelyBitrate (double cutoffHz);
} // namespace wf::fx
