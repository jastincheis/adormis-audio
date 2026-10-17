#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// The effects with a live preview: parametric EQ, compressor, delay and
// reverb. A processor runs on the audio thread for the preview (parameters
// can change while it plays) and offline for Apply.
namespace wf::fx
{
//==============================================================================
struct EqBand
{
    enum Type { bell, lowShelf, highShelf, lowCut, highCut, notch, bandPass };

    Type type = bell;
    double frequency = 1000.0, gainDb = 0.0, q = 1.0;
    int slope = 24;          // dB per octave for the cuts: 12, 24, 36 or 48
    bool on = true;
};

const char* bandTypeName (EqBand::Type);
bool bandHasGain (EqBand::Type);   // bell and shelves

constexpr int maxEqPoints = 32;   // points of the parametric EQ
constexpr int maxEqBands = 34;    // filters in one EQ (a graphic EQ's 31 bands and two shelves at its ends)

struct EqParams
{
    std::array<EqBand, maxEqBands> bands {};
    int count = 0;
    double outputDb = 0.0;
};

// The whole curve at `hz` (dB), and one band's.
double eqResponseDb (const EqParams&, double hz, double sampleRate);
double bandResponseDb (const EqBand&, double hz, double sampleRate);

// Graphic EQ: a row of sliders at fixed frequencies (5, 10, 15, 20 or 31 of
// them; 31 = the third-octave ISO set), each a bell a little wider than the
// space between the sliders, and a shelf at each end. Neighbouring bells add
// up, so their own gains are solved for the curve to pass through every
// slider's setting at its frequency; a slider only reaches its neighbours.
// Bands above what the sample rate holds are left out.
std::vector<double> graphicEqFrequencies (int bands);
EqParams graphicEqParams (const std::vector<double>& hz, const std::vector<double>& sliderDb, double outputDb, double sampleRate);

// Volume envelope (Graphic Fade): a line of points over the selection, time
// 0..1 from its start to its end, gain as a factor (1 = unchanged, 0 =
// silence, up to 4 = +12 dB). Between two points the line is a Bezier curve,
// as in drawing programs: each point has a handle on each side (offsets in
// time and gain). A point that was never curved aims its handles at its
// neighbours, so the lines stay straight when it is moved.
struct EnvelopePoint
{
    double time = 0.0, gain = 1.0;
    bool curved = false;   // false: straight lines to the neighbours
    bool smooth = true;    // the two handles stay in line (a corner when false)
    double inTime = 0.0, inGain = 0.0, outTime = 0.0, outGain = 0.0;
};

constexpr int maxEnvelopePoints = 256;

struct EnvelopeParams
{
    std::array<EnvelopePoint, maxEnvelopePoints> points {};
    int count = 0;
    juce::int64 length = 1;   // samples the envelope spans (the selection)
};

double envelopeGainAt (const EnvelopeParams&, double time);   // time 0..1

// Where point i's handles are (absolute time / gain): the ones of a straight
// point lie on the lines to its neighbours, a third of the way.
void envelopeHandles (const EnvelopeParams&, int i, double& inTime, double& inGain, double& outTime, double& outGain);

struct CompressorParams
{
    double thresholdDb = -18.0, ratio = 4.0, kneeDb = 6.0;
    double attackMs = 10.0, releaseMs = 120.0, makeupDb = 0.0;
    bool autoMakeup = false;
    bool rms = false;       // level detection: peak (fast) or RMS (smoother)
};

// Output level for an input level (dB), with the knee; no makeup.
double compressorCurveDb (const CompressorParams&, double inputDb);
double autoMakeupDb (const CompressorParams&);

struct DelayParams
{
    double timeMs = 375.0;
    bool sync = false;      // time from the tempo and a note value
    double bpm = 120.0;
    int note = 3;           // index into delayNotes()
    double feedback = 0.35, mix = 0.3;
    bool pingPong = false;
    double lowCutHz = 80.0, highCutHz = 9000.0;   // on the echoes

    double effectiveTimeMs() const;
};

struct DelayNote { const char* name; double beats; };
const std::vector<DelayNote>& delayNotes();   // 1/2, 1/4, 1/4 dotted, 1/8 ...

struct ReverbParams
{
    double roomSize = 0.6, damping = 0.45, width = 1.0;
    double preDelayMs = 15.0, lowCutHz = 150.0, highCutHz = 9000.0;
    double wet = 0.3, dry = 1.0;
    bool freeze = false;
};

double suggestedTailSeconds (const DelayParams&);
double suggestedTailSeconds (const ReverbParams&);

//==============================================================================
// Runs on the audio thread (preview) or offline (Apply). Parameters are
// handed over without blocking the audio thread.
class Processor
{
public:
    virtual ~Processor() = default;
    virtual void prepare (double sampleRate, int channels, int maxBlock) = 0;
    virtual void reset() = 0;
    virtual void process (juce::AudioBuffer<float>& audio, int start, int numSamples) = 0;

    // How many samples late the output comes out (block-based processors);
    // render() takes it off so the result lines up with the original.
    virtual int latencySamples() const   { return 0; }

    // Compressor meters (dB), read by the window.
    std::atomic<float> inputLevelDb { -100.0f }, outputLevelDb { -100.0f }, gainReductionDb { 0.0f };
};

std::shared_ptr<Processor> makeEq();
std::shared_ptr<Processor> makeEnvelope();
juce::int64 envelopePosition (const Processor&);   // where the preview is (samples into the selection)
std::shared_ptr<Processor> makeCompressor();
std::shared_ptr<Processor> makeDelay();
std::shared_ptr<Processor> makeReverb();

// Parameters for a processor made above (the type must match).
void setParams (Processor&, const EqParams&);
void setParams (Processor&, const EnvelopeParams&);
void setParams (Processor&, const CompressorParams&);
void setParams (Processor&, const DelayParams&);
void setParams (Processor&, const ReverbParams&);

// The audio through `processor` (prepared here), followed by `tailSamples`
// of what rings on after it. `progress` returns false to cancel.
std::optional<juce::AudioBuffer<float>> render (Processor& processor, const juce::AudioBuffer<float>& audio, double sampleRate,
                                                int tailSamples, const std::function<bool (float)>& progress);
} // namespace wf::fx
