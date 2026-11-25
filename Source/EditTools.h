#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <optional>

// Small editing tools from the Tools and Effects menus.
namespace wf::edittools
{
// Tools > Repair > Interpolate: `range` redrawn as a smooth curve from the
// sample before it to the sample after it (for a click). Returns the new
// audio for `range` (all channels).
juce::AudioBuffer<float> interpolate (const juce::AudioBuffer<float>& audio, juce::Range<int> range);

// Tools > Repair > Replace: `range` filled with the audio just before it
// (after it when there isn't enough before), crossfaded in and out over a
// few milliseconds.
juce::AudioBuffer<float> replaceWithNeighbour (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double sampleRate);

// Effects > Gapper/Snipper over `part`: every `everySeconds`, either a gap of
// silence `lengthSeconds` long goes in (gapper: longer), or that much is cut
// out (snipper: shorter). Edges get 1 ms fades so nothing clicks.
juce::AudioBuffer<float> gapperSnipper (const juce::AudioBuffer<float>& part, double sampleRate, bool snip,
                                        double everySeconds, double lengthSeconds);

// Tools > Find: the next place after `from` (channel -1 = any).
enum class FindWhat { glitch, silence, loud };
struct FindSettings
{
    FindWhat what = FindWhat::glitch;
    double jump = 0.2;          // glitch: a jump this big (fraction of full scale) beyond the trend
    double levelDb = -50.0;     // silence: quieter than this; loud: louder than this
    double minSeconds = 0.2;    // silence: at least this long
};
std::optional<juce::Range<juce::int64>> find (const juce::AudioBuffer<float>& audio, double sampleRate, juce::int64 from,
                                              const FindSettings&);
}
