#include "BassFinder.h"
#include "DarkTheme.h"
#include "Analysis.h"
#include "BassGenerator.h"
#include "NoteDetector.h"
#include "SpaceKey.h"

namespace wf
{
//==============================================================================
LayeredPlayer::LayeredPlayer (juce::AudioBuffer<float> main, juce::AudioBuffer<float> layer, bool loop)
    : mainAudio (std::move (main)), layerAudio (std::move (layer)), looping (loop),
      length (std::max (mainAudio.getNumSamples(), layerAudio.getNumSamples()))
{
}

void LayeredPlayer::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    if (length <= 0)
        return;

    const auto gMain = mainGain.load(), gLayer = layerGain.load();
    auto pos = position.load();

    for (int i = 0; i < info.numSamples; ++i, ++pos)
    {
        auto p = pos;

        if (looping)
            p %= length;
        else if (p >= length)
            break;

        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
        {
            float v = 0.0f;

            if (p < mainAudio.getNumSamples() && mainAudio.getNumChannels() > 0)
                v += gMain * mainAudio.getSample (std::min (ch, mainAudio.getNumChannels() - 1), (int) p);

            if (p < layerAudio.getNumSamples() && layerAudio.getNumChannels() > 0)
                v += gLayer * layerAudio.getSample (std::min (ch, layerAudio.getNumChannels() - 1), (int) p);

            info.buffer->setSample (ch, info.startSample + i, v);
        }
    }

    position = pos;
}

namespace
{
//==============================================================================
namespace colours = dark::colours;
using dark::DarkLook;

juce::Colour statusColour (TuneStatus s)
{
    return s == TuneStatus::ok ? colours::ok : s == TuneStatus::warn ? colours::warn : colours::bad;
}

juce::String formatTime (double t)
{
    t = std::max (0.0, t);
    const auto m = (int) std::floor (t / 60.0);
    return juce::String (m) + ":" + juce::String (t - m * 60.0, 2).paddedLeft ('0', 5);
}

juce::String formatNumber (double value, int decimals)
{
    auto text = juce::String (value, decimals);

    while (text.containsChar ('.') && (text.endsWithChar ('0') || text.endsWithChar ('.')))
        text = text.dropLastCharacters (1);

    return text;
}

juce::String keyLabel (int tonic, bool minor)
{
    const auto flats = keyUsesFlats (tonic, minor);
    return keyName (tonic, minor) + " / " + romanianNoteName (tonic, flats) + (minor ? " minor" : " major");
}

void styleLabel (juce::Label& l, float size, bool bold = false, juce::Colour colour = colours::ink)
{
    l.setFont (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour (juce::Label::textColourId, colour);
    l.setMinimumHorizontalScale (0.7f);
}

// A row of buttons of which one is chosen (the page's "seg" control).
class Segmented final : public juce::Component
{
public:
    explicit Segmented (juce::StringArray labelsIn) : labels (std::move (labelsIn)) {}

    std::function<void (int)> onChange;

    void setSelected (int index)   { selected = index; repaint(); }
    int getSelected() const        { return selected; }
    void setLabels (juce::StringArray l)   { if (l != labels) { labels = std::move (l); repaint(); } }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (colours::surface);
        g.fillRoundedRectangle (area, 6.0f);
        const auto w = area.getWidth() / (float) labels.size();

        for (int i = 0; i < labels.size(); ++i)
        {
            const auto r = juce::Rectangle<float> (area.getX() + w * (float) i, area.getY(), w, area.getHeight());

            if (i == selected)
            {
                g.setColour (colours::accent.withAlpha (0.12f));
                g.fillRoundedRectangle (r.reduced (2.0f), 5.0f);
                g.setColour (colours::accent);
                g.drawRoundedRectangle (r.reduced (2.0f), 5.0f, 1.2f);
            }

            g.setColour (isEnabled() ? (i == selected ? colours::accent : colours::ink) : colours::muted.withAlpha (0.6f));
            g.setFont (juce::FontOptions (13.0f, i == selected ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (labels[i], r.toNearestInt().reduced (4, 0), juce::Justification::centred, 1, 0.7f);
        }

        g.setColour (colours::line);
        g.drawRoundedRectangle (area, 6.0f, 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || labels.isEmpty())
            return;

        const auto index = juce::jlimit (0, labels.size() - 1, e.x * labels.size() / std::max (1, getWidth()));

        if (index != selected)
        {
            setSelected (index);

            if (onChange != nullptr)
                onChange (index);
        }
    }

private:
    juce::StringArray labels;
    int selected = 0;
};

// A white rounded card behind a group of controls.
void paintCard (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (colours::surface);
    g.fillRoundedRectangle (r.toFloat(), 10.0f);
    g.setColour (colours::line);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
}

juce::AudioBuffer<float> toneBuffer (double hz, double rate)
{
    // The page's "play this note": the note with two soft overtones, 1.6 s.
    const auto length = (int) (1.7 * rate);
    juce::AudioBuffer<float> b (2, length);

    for (int i = 0; i < length; ++i)
    {
        const auto t = i / rate;
        const auto env = t < 0.01 ? t / 0.01 * 0.5 : 0.5 * std::pow (0.002, (t - 0.01) / 1.59);
        double v = 0.0;

        for (auto [h, a] : { std::pair { 1.0, 1.0 }, std::pair { 2.0, 0.35 }, std::pair { 3.0, 0.12 } })
            v += a * std::sin (juce::MathConstants<double>::twoPi * hz * h * t);

        b.setSample (0, i, (float) (v * env));
        b.setSample (1, i, (float) (v * env));
    }

    return b;
}

juce::AudioBuffer<float> slice (const juce::AudioBuffer<float>& audio, int start, int end)
{
    start = juce::jlimit (0, audio.getNumSamples(), start);
    end = juce::jlimit (start, audio.getNumSamples(), end);
    juce::AudioBuffer<float> out (audio.getNumChannels(), end - start);

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        out.copyFrom (ch, 0, audio, ch, start, end - start);

    return out;
}

//==============================================================================
// Everything the three tabs share: the audio, its analysis and the key in use.
struct Model
{
    BassFinderSource source;
    BassFinderHost host;

    std::optional<KeyAnalysis> analysis;
    std::optional<TempoEstimate> tempo;
    bool tempoFromFile = false;   // the tempo is the file's, not detected here
    int keyTonic = 5;       // the key the detector and the generator use
    bool minor = true;

    // Which part of the window is playing, and where its audio starts in the source.
    enum class Playing { nothing, tone, audio, preview };
    Playing playing = Playing::nothing;
    double playOffsetSeconds = 0.0;
    juce::int64 playLength = 0;

    std::function<void()> keyChanged;      // the generator's key combo changed it
    std::function<void()> sourceChanged;   // "Analyse again"

    double duration() const  { return source.audio.getNumSamples() / source.sampleRate; }

    void analyse()
    {
        const juce::Range<int> all { 0, source.audio.getNumSamples() };
        analysis = analyseKey (source.audio, all, source.sampleRate);
        tempo = detectTempo (source.audio, all, source.sampleRate);
        tempoFromFile = source.fileBpm > 0.0;

        // The file's own tempo wins over the detection here; the first beat
        // still comes from the audio.
        if (tempoFromFile)
        {
            if (! tempo.has_value())
                tempo = TempoEstimate { source.fileBpm, 0, 0.0, 1.0 };

            tempo->bpm = source.fileBpm;
            tempo->confidence = 1.0;
            const auto bars = duration() * source.fileBpm / 240.0;
            tempo->loopBars = std::abs (bars - std::round (bars)) < 0.03 ? (int) std::round (bars) : 0;
        }

        if (analysis.has_value() && ! analysis->keys.empty())
        {
            keyTonic = analysis->keys[0].tonic;
            minor = analysis->keys[0].minor;
        }
    }

    void play (juce::AudioBuffer<float> main, juce::AudioBuffer<float> layer, bool loop, Playing kind, double offset,
               LayeredPlayer** created = nullptr)
    {
        if (host.play == nullptr)
            return;

        const auto channels = std::max (main.getNumChannels(), layer.getNumChannels());
        auto player = std::make_unique<LayeredPlayer> (std::move (main), std::move (layer), loop);
        playLength = player->getLength();

        if (created != nullptr)
            *created = player.get();

        playing = kind;
        playOffsetSeconds = offset;
        host.play (std::move (player), source.sampleRate, std::max (1, channels));
    }

    void stop()
    {
        if (playing != Playing::nothing && host.stop != nullptr)
            host.stop();

        playing = Playing::nothing;
    }

    // Seconds into the source of what is playing now; empty when nothing of ours plays.
    std::optional<double> playSeconds() const
    {
        if (playing == Playing::nothing || host.playPosition == nullptr)
            return {};

        const auto p = host.playPosition();

        if (p < 0)
            return {};

        return playOffsetSeconds + (double) p / source.sampleRate;
    }
};

//==============================================================================
// Energy per note: 12 bars, the tonic in the bass colour, scale notes in the accent.
class ChromaChart final : public juce::Component
{
public:
    explicit ChromaChart (Model& m) : model (m) {}

    void paint (juce::Graphics& g) override
    {
        if (! model.analysis.has_value())
            return;

        const auto& chroma = model.analysis->chroma;
        const auto maxValue = std::max (1.0e-9, *std::max_element (chroma.begin(), chroma.end()));
        const auto flats = keyUsesFlats (model.keyTonic, model.minor);
        auto area = getLocalBounds().toFloat();
        const auto labels = area.removeFromBottom (18.0f);
        const auto w = area.getWidth() / 12.0f;

        for (int pc = 0; pc < 12; ++pc)
        {
            const auto inScale = isInKey (pc, model.keyTonic, model.minor);
            const auto colour = pc == model.keyTonic ? colours::bass : inScale ? colours::accent : colours::muted.withAlpha (0.45f);
            const auto h = std::max (2.0f, area.getHeight() * 0.95f * (float) (chroma[(size_t) pc] / maxValue));
            const auto x = area.getX() + w * (float) pc;

            g.setColour (colours::line.withAlpha (0.4f));
            g.fillRoundedRectangle (x + 4.0f, area.getY(), w - 8.0f, area.getHeight(), 4.0f);
            g.setColour (colour);
            g.fillRoundedRectangle (x + 4.0f, area.getBottom() - h, w - 8.0f, h, 4.0f);

            g.setColour (pc == model.keyTonic ? colours::bass : colours::ink);
            g.setFont (juce::FontOptions (12.0f, pc == model.keyTonic ? juce::Font::bold : juce::Font::plain));
            g.drawText (noteName (pc, flats), juce::Rectangle<float> (x, labels.getY(), w, labels.getHeight()),
                        juce::Justification::centred);
        }
    }

private:
    Model& model;
};

// The five most likely keys with a bar each.
class CandidateList final : public juce::Component
{
public:
    explicit CandidateList (Model& m) : model (m) {}

    void paint (juce::Graphics& g) override
    {
        if (! model.analysis.has_value() || model.analysis->keys.size() < 2)
            return;

        const auto& keys = model.analysis->keys;
        const auto best = keys.front().score, worst = keys.back().score;
        auto area = getLocalBounds();
        const auto rowHeight = juce::jlimit (24, 40, area.getHeight() / 5);

        for (size_t i = 0; i < std::min ((size_t) 5, keys.size()); ++i)
        {
            auto row = area.removeFromTop (rowHeight).reduced (0, 2);
            const auto& k = keys[i];
            const auto p = juce::jlimit (0.0f, 1.0f, (float) ((k.score - worst) / std::max (1.0e-9, best - worst)));
            auto text = row.removeFromTop (row.getHeight() - 7);

            g.setColour (colours::ink);
            g.setFont (juce::FontOptions (13.0f, i == 0 ? juce::Font::bold : juce::Font::plain));
            g.drawText (keyName (k.tonic, k.minor) + "  -  bass in " + noteName (k.tonic, keyUsesFlats (k.tonic, k.minor)),
                        text, juce::Justification::centredLeft);
            g.setColour (colours::muted);
            g.drawText (juce::String (juce::roundToInt (k.score / 1.35 * 100.0)) + "%", text, juce::Justification::centredRight);

            auto bar = row.toFloat().withHeight (4.0f).translated (0.0f, 1.0f);
            g.setColour (colours::line);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (i == 0 ? colours::bass : colours::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * p), 2.0f);
        }
    }

private:
    Model& model;
};

//==============================================================================
// Tab 1: key, tuning, tempo and which note to tune the bass to.
class KeyPanel final : public juce::Component
{
public:
    explicit KeyPanel (Model& m) : model (m), chroma (m), candidates (m)
    {
        for (auto* l : { &bigNote, &keyText, &chips, &recoTitle, &freq1, &freq2, &safeNotes, &bassNow, &chromaTitle,
                         &legend, &candidatesTitle, &note })
            addAndMakeVisible (l);

        styleLabel (bigNote, 76.0f, true, colours::bass);
        bigNote.setJustificationType (juce::Justification::centred);
        styleLabel (keyText, 22.0f, true);
        styleLabel (chips, 14.0f, false, colours::muted);
        styleLabel (recoTitle, 15.0f, true);
        styleLabel (freq1, 15.0f);
        styleLabel (freq2, 15.0f);
        styleLabel (safeNotes, 14.0f);
        styleLabel (bassNow, 13.0f, false, colours::muted);
        styleLabel (chromaTitle, 15.0f, true);
        styleLabel (legend, 12.0f, false, colours::muted);
        styleLabel (candidatesTitle, 15.0f, true);
        styleLabel (note, 12.0f, false, colours::muted);
        safeNotes.setJustificationType (juce::Justification::topLeft);
        note.setJustificationType (juce::Justification::topLeft);

        recoTitle.setText ("Bass recommendation", juce::dontSendNotification);
        chromaTitle.setText ("Energy per note", juce::dontSendNotification);
        legend.setText ("orange = tonic, blue = in the scale, grey = outside the scale", juce::dontSendNotification);
        candidatesTitle.setText ("Other possible keys", juce::dontSendNotification);
        note.setText ("The key is guessed from the whole selection (or the whole file). Choose another one in "
                      "MIDI Bass if your ear says otherwise: the note detector and the generator follow it.",
                      juce::dontSendNotification);

        for (auto* b : { &play1, &play2 })
        {
            b->setColour (juce::TextButton::buttonColourId, colours::bass.withAlpha (0.15f));
            b->setColour (juce::TextButton::textColourOffId, colours::bass);
            addAndMakeVisible (b);
        }

        play1.onClick = [this] { playOctave (1); };
        play2.onClick = [this] { playOctave (2); };

        addAndMakeVisible (chroma);
        addAndMakeVisible (candidates);
        refresh();
    }

    void refresh()
    {
        if (! model.analysis.has_value() || model.analysis->keys.size() < 2)
        {
            bigNote.setText ("-", juce::dontSendNotification);
            keyText.setText ("No clear notes in this audio", juce::dontSendNotification);
            chips.setText ({}, juce::dontSendNotification);
            freq1.setText ({}, juce::dontSendNotification);
            freq2.setText ({}, juce::dontSendNotification);
            safeNotes.setText ({}, juce::dontSendNotification);
            bassNow.setText ({}, juce::dontSendNotification);
            repaint();
            return;
        }

        const auto& a = *model.analysis;
        const auto& top = a.keys[0];
        const auto flats = keyUsesFlats (top.tonic, top.minor);
        const auto gap = top.score - a.keys[1].score;

        bigNote.setText (noteName (top.tonic, flats).replace ("#", juce::CharPointer_UTF8 ("\xe2\x99\xaf"))
                                                     .replace ("b", juce::CharPointer_UTF8 ("\xe2\x99\xad")),
                         juce::dontSendNotification);
        keyText.setText (keyLabel (top.tonic, top.minor), juce::dontSendNotification);

        // gap < 0: the profiles preferred the parallel key (the third over the tonic
        // decided) or the relative key (the chord the music starts on decided)
        const auto& second = a.keys[1];
        const auto relative = second.minor != top.minor && second.tonic == (top.tonic + (top.minor ? 3 : 9)) % 12;
        auto confidence = gap < 0.0 && ! relative ? (top.minor ? "minor by its third (" : "major by its third (")
                                                        + noteName (top.tonic + (top.minor ? 3 : 4), flats) + "), check by ear"
                        : gap < 0.0 ? juce::String ("starts on ") + noteName (top.tonic, flats) + (top.minor ? "m" : "") + ", check by ear"
                        : gap > 0.12 ? juce::String ("sure") : gap > 0.04 ? juce::String ("probably") : juce::String ("ambiguous, check by ear");

        if (relative && gap < 0.35)
            confidence << " (same notes as " << keyName (second.tonic, second.minor) << ")";
        const auto tuning = a.tuningCents == 0 ? juce::String ("tuned to A440")
                                               : (a.tuningCents > 0 ? "+" : "") + juce::String (a.tuningCents) + " cents from A440";
        auto chipText = juce::String (confidence) + "   |   " + tuning;

        if (model.tempo.has_value())
            chipText << "   |   " << formatNumber (model.tempo->bpm, 2) << " BPM";

        chips.setText (chipText, juce::dontSendNotification);

        for (int octave : { 1, 2 })
        {
            const auto hz = noteFrequency (top.tonic, octave, a.tuningCents);
            (octave == 1 ? freq1 : freq2).setText (noteName (top.tonic, flats) + juce::String (octave) + "  -  "
                                                       + juce::String (hz, 2) + " Hz   "
                                                       + (octave == 1 ? "sub / 808" : "classic bass, an octave up"),
                                                   juce::dontSendNotification);
        }

        const std::vector<std::pair<int, const char*>> intervals = top.minor
            ? std::vector<std::pair<int, const char*>> { { 0, "tonic" }, { 7, "fifth" }, { 8, "minor sixth" }, { 10, "minor seventh" }, { 3, "minor third" } }
            : std::vector<std::pair<int, const char*>> { { 0, "tonic" }, { 7, "fifth" }, { 5, "fourth" }, { 9, "sixth (relative minor)" }, { 4, "third" } };

        juce::StringArray parts;

        for (auto& [i, label] : intervals)
            parts.add (noteName (top.tonic + i, flats) + " (" + label + ")");

        safeNotes.setText ("Safe notes for the bass line: " + parts.joinIntoString (", ") + ".", juce::dontSendNotification);

        const auto bassMax = std::max_element (a.bass.begin(), a.bass.end());
        bassNow.setText (*bassMax > 0.0 ? "Strongest note in the bass: " + noteName ((int) (bassMax - a.bass.begin()), flats)
                                        : juce::String(),
                         juce::dontSendNotification);
        repaint();
        chroma.repaint();
        candidates.repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::bg);
        paintCard (g, verdictCard);
        paintCard (g, chromaCard);
        paintCard (g, candidatesCard);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        verdictCard = area.removeFromTop (250);
        area.removeFromTop (12);

        auto inner = verdictCard.reduced (16);
        auto left = inner.removeFromLeft (150);
        bigNote.setBounds (left.removeFromTop (110));
        inner.removeFromLeft (12);
        keyText.setBounds (inner.removeFromTop (30));
        chips.setBounds (inner.removeFromTop (24));
        inner.removeFromTop (8);
        recoTitle.setBounds (inner.removeFromTop (24));

        for (auto [label, button] : { std::pair { &freq1, &play1 }, std::pair { &freq2, &play2 } })
        {
            auto row = inner.removeFromTop (32);
            button->setBounds (row.removeFromRight (44).reduced (2));
            label->setBounds (row);
        }

        safeNotes.setBounds (inner.removeFromTop (44));
        bassNow.setBounds (inner.removeFromTop (20));

        auto bottom = area;
        chromaCard = bottom.removeFromLeft (bottom.getWidth() * 3 / 5);
        bottom.removeFromLeft (12);
        candidatesCard = bottom;

        auto c = chromaCard.reduced (14);
        chromaTitle.setBounds (c.removeFromTop (24));
        legend.setBounds (c.removeFromBottom (20));
        chroma.setBounds (c);

        auto k = candidatesCard.reduced (14);
        candidatesTitle.setBounds (k.removeFromTop (24));
        note.setBounds (k.removeFromBottom (56));
        candidates.setBounds (k);
    }

private:
    void playOctave (int octave)
    {
        if (! model.analysis.has_value() || model.analysis->keys.empty())
            return;

        model.stop();
        const auto hz = noteFrequency (model.analysis->keys[0].tonic, octave, model.analysis->tuningCents);
        model.play (toneBuffer (hz, model.source.sampleRate), {}, false, Model::Playing::tone, 0.0);
    }

    Model& model;
    juce::Label bigNote, keyText, chips, recoTitle, freq1, freq2, safeNotes, bassNow, chromaTitle, legend, candidatesTitle, note;
    juce::TextButton play1 { juce::CharPointer_UTF8 ("\xe2\x96\xb6") }, play2 { juce::CharPointer_UTF8 ("\xe2\x96\xb6") };
    ChromaChart chroma;
    CandidateList candidates;
    juce::Rectangle<int> verdictCard, chromaCard, candidatesCard;
};

//==============================================================================
// State of the note detector tab.
struct DetectorState
{
    NoteSource source = NoteSource::bass;
    bool songReference = true;   // cents against the recording's own tuning, not A = 440 Hz
    int tolerance = 10;
    bool onlyProblems = false;
    std::optional<NoteTrack> track;
    int selected = -1;
    bool listenCorrected = false;
    std::optional<juce::AudioBuffer<float>> corrected;   // rendered shifts; empty = needs rendering
    AutoCorrect autoSettings;

    bool hasShifts() const
    {
        return track.has_value() && std::any_of (track->notes.begin(), track->notes.end(), [] (auto& n) { return n.shift != 0; });
    }
};

// The detected notes over time on a piano-roll grid: grey raw pitch trace,
// coloured notes (green in tolerance, yellow up to 25 cents, red beyond),
// a red frame around notes outside the key. Click a note to hear it.
class PitchGraph final : public juce::Component,
                         private juce::ScrollBar::Listener
{
public:
    PitchGraph (Model& m, DetectorState& s) : model (m), state (s)
    {
        scrollBar.setAutoHide (false);
        scrollBar.setColour (juce::ScrollBar::thumbColourId, colours::muted.withAlpha (0.6f));
        scrollBar.setWantsKeyboardFocus (false);
        scrollBar.addListener (this);
        addChildComponent (scrollBar);
    }

    std::function<void (int)> onNoteClicked;
    std::function<void (double)> onTimeClicked;
    std::function<void (int note, int shift)> onNoteDragged;   // dragged up / down: its new correction (cents)
    std::function<void (int)> onNoteDropped;

    void setPlayhead (std::optional<double> t)
    {
        if (t != playhead)
        {
            playhead = t;

            // Zoomed in: turn the page when the playhead leaves the view.
            const auto length = shownLength();

            if (t.has_value() && zoomed() && (*t < viewStart || *t > viewStart + length))
                setViewStart (*t - 0.05 * length);

            repaint();
        }
    }

    // Up / Down in the panel: halves or doubles the stretch shown, keeping
    // the playhead (or the clicked place) where it is on screen.
    void zoom (bool in)
    {
        const auto oldLength = shownLength();
        auto anchor = viewStart + oldLength / 2.0;

        if (playhead.has_value() && *playhead >= viewStart && *playhead <= viewStart + oldLength)
            anchor = *playhead;
        else if (cursor.has_value() && *cursor >= viewStart && *cursor <= viewStart + oldLength)
            anchor = *cursor;

        zoomAround (anchor, in ? 0.5 : 2.0);
    }

    // `factor` < 1 shows less (zooms in); `anchor` stays where it is on screen.
    void zoomAround (double anchor, double factor)
    {
        const auto duration = model.duration();

        if (duration <= 0.0)
            return;

        const auto oldLength = shownLength();
        const auto newLength = juce::jlimit (std::min (minViewSeconds, duration), duration, oldLength * factor);
        const auto fraction = (anchor - viewStart) / oldLength;
        viewLength = newLength;
        setViewStart (anchor - fraction * newLength);
    }

    // Where Space starts playing: the clicked place if it is in view, else
    // the start of what is shown.
    double playStart() const
    {
        if (cursor.has_value() && *cursor >= viewStart && *cursor < viewStart + shownLength())
            return *cursor;

        return viewStart;
    }

    void resetView()
    {
        viewStart = 0.0;
        viewLength = 0.0;
        cursor.reset();
        updateScrollBar();
        repaint();
    }

    void resized() override
    {
        scrollBar.setBounds ((int) left, getHeight() - scrollBarHeight, getWidth() - (int) left, scrollBarHeight);
    }

    // The wheel zooms around the mouse; Shift + wheel (or sideways) scrolls.
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (model.duration() <= 0.0)
            return;

        const auto sideways = std::abs (wheel.deltaX) > std::abs (wheel.deltaY);

        if (sideways || e.mods.isShiftDown())
        {
            if (zoomed())
            {
                const auto delta = sideways ? -wheel.deltaX : -wheel.deltaY;
                setViewStart (viewStart + delta * shownLength() * 0.5);
            }
            return;
        }

        if (wheel.deltaY != 0.0f)
            zoomAround (timeAt (e.x), std::pow (2.0, (double) -wheel.deltaY * 2.0));
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (bounds, 6.0f);

        const auto duration = model.duration();

        if (duration <= 0.0)
            return;

        int lo = 1000, hi = -1000;

        if (state.track.has_value())
            for (auto& n : state.track->notes)
            {
                const auto moved = n.corrected().first;
                lo = std::min ({ lo, n.note, moved });
                hi = std::max ({ hi, n.note, moved });
            }

        if (lo > hi)
        {
            lo = 36;
            hi = 48;
        }

        lo -= 2;
        hi += 2;

        if (hi - lo < 10)
        {
            const auto c = (hi + lo) / 2.0;
            lo = (int) std::floor (c - 5);
            hi = (int) std::ceil (c + 5);
        }

        // While a note is dragged the rows stay put under the mouse.
        if (dragged >= 0)
        {
            lo = dragLo;
            hi = dragHi;
        }

        shownLo = lo;
        shownHi = hi;

        const auto rows = hi - lo + 1;
        const auto h = bounds.getHeight() - (zoomed() ? (float) scrollBarHeight : 0.0f), w = bounds.getWidth();
        const auto rowHeight = h / (float) rows;
        const auto length = shownLength();
        auto X = [&] (double t) { return (float) (left + (t - viewStart) / length * (w - left)); };
        auto Y = [&] (double m) { return (float) (h - (m - lo + 0.5) * rowHeight); };
        const auto flats = keyUsesFlats (model.keyTonic, model.minor);

        g.setFont (juce::FontOptions (10.0f));

        for (int n = lo; n <= hi; ++n)
        {
            const auto pc = ((n % 12) + 12) % 12;

            if (isInKey (pc, model.keyTonic, model.minor))
            {
                g.setColour (colours::surface);
                g.fillRect (left, Y (n) - rowHeight / 2.0f, w - left, rowHeight);
            }

            if (rowHeight >= 9.0f || pc == model.keyTonic)
            {
                g.setColour (pc == model.keyTonic ? colours::bass : colours::muted);
                g.drawText (midiNoteName (n, flats), juce::Rectangle<float> (2.0f, Y (n) - 6.0f, left - 2.0f, 12.0f),
                            juce::Justification::centredLeft);
            }
        }

        g.setColour (colours::muted.withAlpha (0.7f));
        g.drawText (juce::CharPointer_UTF8 ("Wheel, Up / Down: zoom   Drag a note up / down: move it (Shift: in cents)   Space: play / stop"),
                    juce::Rectangle<float> (left, h - 15.0f, w - left - 6.0f, 13.0f), juce::Justification::centredRight);

        // Everything below is drawn right of the note names only.
        juce::Graphics::ScopedSaveState keepClip (g);
        g.reduceClipRegion (juce::Rectangle<float> (left, 0.0f, w - left, h).toNearestInt());

        // Bar lines.
        if (model.tempo.has_value() && model.tempo->bpm > 0.0)
        {
            g.setColour (colours::line);

            for (auto t = model.tempo->downbeat; t < duration; t += 240.0 / model.tempo->bpm)
                g.fillRect (std::round (X (t)), 0.0f, 1.0f, h);
        }

        if (! state.track.has_value())
            return;

        // Stretches with no single clear note (two notes at once?).
        for (auto& r : state.track->unclear)
        {
            const auto x = X (r.getStart()), ww = std::max (2.0f, X (r.getEnd()) - x);
            g.setColour (colours::warn.withAlpha (0.12f));
            g.fillRect (x, 0.0f, ww, h);

            if (ww > 60.0f)
            {
                g.setColour (colours::warn);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText ("2+ notes?", juce::Rectangle<float> (x + 4.0f, 4.0f, ww - 8.0f, 14.0f), juce::Justification::centredLeft);
            }
        }

        // Raw pitch trace.
        juce::Path trace;
        bool pen = false;

        for (auto& f : state.track->frames)
        {
            if (! f.midi.has_value() || *f.midi < lo - 0.5 || *f.midi > hi + 0.5)
            {
                pen = false;
                continue;
            }

            if (pen)
                trace.lineTo (X (f.time), Y (*f.midi));
            else
                trace.startNewSubPath (X (f.time), Y (*f.midi));

            pen = true;
        }

        g.setColour (colours::muted.withAlpha (0.5f));
        g.strokePath (trace, juce::PathStrokeType (1.0f));

        const auto& notes = state.track->notes;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& s = notes[i];
            const auto [note, cents] = s.corrected();
            const auto status = tuneStatus (cents, state.tolerance);
            const auto x = X (s.start), ww = std::max (2.0f, X (s.end) - x);
            const auto hh = std::min (10.0f, rowHeight * 0.7f);
            const auto y0 = Y (s.note + s.cents / 100.0) - hh / 2.0f, y = Y (note + cents / 100.0) - hh / 2.0f;

            if (s.shift != 0)
            {
                g.setColour (statusColour (tuneStatus (s.cents, state.tolerance)).withAlpha (0.7f));
                const float dashes[] { 3.0f, 2.0f };
                juce::Path frame;
                frame.addRectangle (x + 0.5f, y0 + 0.5f, ww - 1.0f, hh - 1.0f);
                juce::Path dashed;
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, frame, dashes, 2);
                g.fillPath (dashed);
                const auto mx = x + std::min (6.0f, ww / 2.0f);
                g.drawLine (mx, y0 + hh / 2.0f, mx, y + hh / 2.0f, 1.0f);
            }

            g.setColour (statusColour (status).withAlpha ((int) i == state.selected ? 1.0f : 0.85f));
            g.fillRect (x, y, ww, hh);

            if (! isInKey (note, model.keyTonic, model.minor))
            {
                g.setColour (colours::bad);
                g.drawRect (x - 1.5f, y - 1.5f, ww + 3.0f, hh + 3.0f, 1.5f);
            }

            if ((int) i == state.selected)
            {
                g.setColour (colours::ink);
                g.drawRect (x - 2.0f, y - 2.0f, ww + 4.0f, hh + 4.0f, 2.0f);
            }

            if (ww > 34.0f && (status != TuneStatus::ok || s.shift != 0))
            {
                g.setColour (colours::ink);
                g.drawText (s.shift != 0 ? formatCents (s.cents) + juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92")) + formatCents (cents)
                                         : formatCents (cents),
                            juce::Rectangle<float> (x + 2.0f, y - 13.0f, 120.0f, 12.0f), juce::Justification::centredLeft);
            }
        }

        if (cursor.has_value())
        {
            g.setColour (colours::ink.withAlpha (0.55f));
            g.fillRect (X (*cursor) - 0.5f, 0.0f, 1.0f, h);
        }

        if (playhead.has_value())
        {
            g.setColour (colours::accent);
            g.fillRect (X (*playhead) - 1.0f, 0.0f, 2.0f, h);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto duration = model.duration();

        if (duration <= 0.0)
            return;

        const auto t = viewStart + std::max (0.0f, e.x - left) / (getWidth() - left) * shownLength();
        cursor = t;
        repaint();
        int best = -1;
        double bestDistance = 1.0e9;

        if (state.track.has_value())
            for (size_t i = 0; i < state.track->notes.size(); ++i)
            {
                const auto& s = state.track->notes[i];
                const auto d = t < s.start ? s.start - t : t > s.end ? t - s.end : 0.0;

                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = (int) i;
                }
            }

        dragged = -1;
        dragMoved = false;

        if (best >= 0 && bestDistance < 0.25)
        {
            // Pressed on the note itself (in time, near its row): it can be dragged.
            const auto& s = state.track->notes[(size_t) best];
            const auto [note, cents] = s.corrected();

            if (bestDistance == 0.0 && std::abs (e.y - pitchToY (note + cents / 100.0)) < std::max (8.0f, rowHeightShown() * 0.8f))
            {
                dragged = best;
                dragLo = shownLo;
                dragHi = shownHi;
                dragStartY = e.y;
            }

            if (onNoteClicked != nullptr)
                onNoteClicked (best);
        }
        else if (onTimeClicked != nullptr)
        {
            onTimeClicked (std::max (0.0, t));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged < 0 || ! state.track.has_value() || dragged >= (int) state.track->notes.size())
            return;

        if (! dragMoved && std::abs (e.y - dragStartY) < 4)
            return;

        dragMoved = true;
        const auto& s = state.track->notes[(size_t) dragged];
        const auto measured = s.note + s.cents / 100.0;
        const auto target = yToPitch ((float) e.y);
        // Whole notes, exactly in tune; Shift: any number of cents.
        const auto shift = e.mods.isShiftDown() ? (int) std::lround ((target - measured) * 100.0)
                                                : (int) std::lround ((std::round (target) - measured) * 100.0);

        if (onNoteDragged != nullptr)
            onNoteDragged (dragged, juce::jlimit (-maxShiftCents, maxShiftCents, shift));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        const auto note = dragged;
        const auto moved = dragMoved;
        dragged = -1;
        dragMoved = false;

        if (moved && onNoteDropped != nullptr)
            onNoteDropped (note);

        repaint();
    }

    static constexpr int maxShiftCents = 1200;

private:
    double timeAt (int x) const   { return viewStart + std::max (0.0f, x - left) / (getWidth() - left) * shownLength(); }

    float pitchAreaHeight() const { return (float) getHeight() - (zoomed() ? (float) scrollBarHeight : 0.0f); }
    float rowHeightShown() const  { return pitchAreaHeight() / (float) (shownHi - shownLo + 1); }
    float pitchToY (double m) const { return (float) (pitchAreaHeight() - (m - shownLo + 0.5) * rowHeightShown()); }
    double yToPitch (float y) const { return (pitchAreaHeight() - y) / rowHeightShown() + shownLo - 0.5; }

    double shownLength() const
    {
        const auto duration = model.duration();
        return viewLength > 0.0 ? std::min (viewLength, duration) : duration;
    }

    bool zoomed() const   { return shownLength() < model.duration() - 1.0e-9; }

    void setViewStart (double start)
    {
        viewStart = juce::jlimit (0.0, std::max (0.0, model.duration() - shownLength()), start);
        updateScrollBar();
        repaint();
    }

    void updateScrollBar()
    {
        scrollBar.setVisible (zoomed());
        scrollBar.setRangeLimits (0.0, std::max (model.duration(), 1.0e-6), juce::dontSendNotification);
        scrollBar.setCurrentRange (viewStart, shownLength(), juce::dontSendNotification);
    }

    void scrollBarMoved (juce::ScrollBar*, double newStart) override   { setViewStart (newStart); }

    static constexpr float left = 36.0f;
    static constexpr int scrollBarHeight = 10;
    static constexpr double minViewSeconds = 0.5;
    Model& model;
    DetectorState& state;
    std::optional<double> playhead, cursor;   // cursor = last clicked place
    int shownLo = 36, shownHi = 48;            // the rows drawn last
    int dragged = -1, dragLo = 0, dragHi = 0, dragStartY = 0;   // the note being dragged up / down
    bool dragMoved = false;
    double viewStart = 0.0, viewLength = 0.0;   // seconds; length 0 = the whole audio
    juce::ScrollBar scrollBar { false };
};

// The current note and its cents on a needle while playing.
class Tuner final : public juce::Component
{
public:
    void set (std::optional<std::pair<int, int>> noteAndCents, int tolerance, bool flats)
    {
        value = noteAndCents;
        tol = tolerance;
        useFlats = flats;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (area, 8.0f);

        auto noteArea = area.removeFromLeft (110.0f);
        const auto status = value.has_value() ? tuneStatus (value->second, tol) : TuneStatus::ok;

        g.setColour (value.has_value() ? statusColour (status) : colours::muted);
        g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
        g.drawText (value.has_value() ? midiNoteName (value->first, useFlats) : juce::String ("-"), noteArea, juce::Justification::centred);

        auto meter = area.reduced (10.0f, 0.0f);
        auto text = meter.removeFromTop (meter.getHeight() * 0.5f);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.setColour (colours::ink);
        g.drawText (value.has_value() ? formatCents (value->second) : juce::String ("-"), text, juce::Justification::centredLeft);
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::CharPointer_UTF8 ("\xe2\x88\x92" "50\xc2\xa2   \xe2\x80\x93   +50\xc2\xa2"), text, juce::Justification::centredRight);

        auto scale = meter.reduced (0.0f, 6.0f).withHeight (8.0f).translated (0.0f, 4.0f);
        g.setColour (colours::line);
        g.fillRoundedRectangle (scale, 4.0f);
        const auto mid = scale.getCentreX();
        const auto half = scale.getWidth() / 2.0f;
        g.setColour (colours::ok.withAlpha (0.35f));
        g.fillRect (mid - half * (float) tol / 50.0f, scale.getY(), half * 2.0f * (float) tol / 50.0f, scale.getHeight());

        if (value.has_value())
        {
            const auto x = mid + half * (float) juce::jlimit (-50, 50, value->second) / 50.0f;
            g.setColour (statusColour (status));
            g.fillRoundedRectangle (x - 2.0f, scale.getY() - 5.0f, 4.0f, scale.getHeight() + 10.0f, 2.0f);
        }
    }

private:
    std::optional<std::pair<int, int>> value;
    int tol = 10;
    bool useFlats = false;
};

//==============================================================================
// Tab 2: how far off each note is, with manual and automatic correction.
// Buttons, sliders and combo boxes inside `root` stop taking the keyboard
// (text boxes keep it), so keys reach the panel.
static void keepKeysHere (juce::Component& root)
{
    for (auto* c : root.getChildren())
    {
        if (dynamic_cast<juce::Button*> (c) != nullptr || dynamic_cast<juce::ComboBox*> (c) != nullptr
            || dynamic_cast<juce::Slider*> (c) != nullptr)
            c->setWantsKeyboardFocus (false);

        if (dynamic_cast<juce::TextEditor*> (c) == nullptr)
            keepKeysHere (*c);
    }
}

class DetectorPanel final : public juce::Component,
                            private juce::ListBoxModel,
                            private juce::Timer
{
public:
    explicit DetectorPanel (Model& m) : model (m), graph (m, state)
    {
        for (auto* l : { &title, &sourceLabel, &tolLabel, &summary, &status, &editInfo, &editResult, &autoTitle,
                         &strengthLabel, &scopeLabel, &help, &listenLabel })
            addAndMakeVisible (l);

        styleLabel (title, 16.0f, true);
        title.setText ("Note detector - how far off each note is", juce::dontSendNotification);
        styleLabel (sourceLabel, 13.0f, false, colours::muted);
        sourceLabel.setText ("Listen to:", juce::dontSendNotification);
        styleLabel (tolLabel, 13.0f, false, colours::muted);
        tolLabel.setText ("Tolerance:", juce::dontSendNotification);
        styleLabel (referenceLabel, 13.0f, false, colours::muted);
        referenceLabel.setText ("In tune with:", juce::dontSendNotification);
        addAndMakeVisible (referenceLabel);
        styleLabel (summary, 13.0f);
        summary.setJustificationType (juce::Justification::topLeft);
        styleLabel (status, 13.0f, false, colours::muted);
        styleLabel (editInfo, 13.0f);
        styleLabel (editResult, 13.0f, true);
        styleLabel (autoTitle, 14.0f, true);
        autoTitle.setText ("Automatic correction", juce::dontSendNotification);
        styleLabel (strengthLabel, 13.0f, false, colours::muted);
        strengthLabel.setText ("How much:", juce::dontSendNotification);
        styleLabel (scopeLabel, 13.0f, false, colours::muted);
        scopeLabel.setText ("Which notes:", juce::dontSendNotification);
        styleLabel (listenLabel, 13.0f, false, colours::muted);
        listenLabel.setText ("Play:", juce::dontSendNotification);
        styleLabel (help, 12.0f, false, colours::muted);
        help.setJustificationType (juce::Justification::topLeft);
        help.setText (juce::CharPointer_UTF8 ("Cents against the song's own tuning (a recording tuned to A = 432 Hz is in tune with itself), "
                                              "or against standard A = 440 Hz (100 cents = one semitone). Green is within the tolerance, "
                                              "yellow up to \xc2\xb1" "25\xc2\xa2, red beyond. Click a note to hear and correct it; drag it up or down to "
                                              "move it to another note (Shift: in cents). With "
                                              "\"Bass\" only the low band (below ~450 Hz) is moved and the rest of the mix stays untouched; "
                                              "with \"Melody\" the whole signal moves for the length of the note. Timing and length never change. "
                                              "\"Apply to file\" writes the corrections into the file (Ctrl+Z undoes it)."),
                      juce::dontSendNotification);

        source.onChange = [this] (int i) { state.source = i == 0 ? NoteSource::bass : NoteSource::melody; run(); };
        reference.onChange = [this] (int i) { state.songReference = i == 0; run(); };
        tolerance.setSelected (1);
        tolerance.onChange = [this] (int i) { state.tolerance = i == 0 ? 5 : i == 1 ? 10 : 20; refresh(); };
        strength.setSelected (2);
        strength.onChange = [this] (int i) { state.autoSettings.strength = i == 0 ? 50 : i == 1 ? 75 : 100; };
        scope.onChange = [this] (int i) { state.autoSettings.allNotes = i == 1; };
        listen.onChange = [this] (int i) { setListenCorrected (i == 1); };

        for (auto* c : { (juce::Component*) &source, (juce::Component*) &reference, (juce::Component*) &tolerance, (juce::Component*) &strength,
                         (juce::Component*) &scope, (juce::Component*) &listen })
            addAndMakeVisible (c);

        for (auto* t : { &onlyProblems, &snapToKey })
        {
            t->setClickingTogglesState (true);
            t->setColour (juce::TextButton::buttonOnColourId, colours::accent.withAlpha (0.18f));
            t->setColour (juce::TextButton::textColourOnId, colours::accent);
            addAndMakeVisible (t);
        }

        onlyProblems.onClick = [this] { state.onlyProblems = onlyProblems.getToggleState(); refresh(); };
        snapToKey.onClick = [this] { state.autoSettings.snapToKey = snapToKey.getToggleState(); };

        slider.setRange (-100.0, 100.0, 1.0);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
        slider.setTextValueSuffix (juce::CharPointer_UTF8 ("\xc2\xa2"));
        slider.onValueChange = [this] { setShift ((int) slider.getValue()); };
        addAndMakeVisible (slider);

        for (auto [button, delta] : { std::pair { &minus10, -10 }, std::pair { &minus1, -1 }, std::pair { &plus1, 1 }, std::pair { &plus10, 10 } })
        {
            button->onClick = [this, d = delta]
            {
                if (auto* n = selectedNote())
                    setShift (n->shift + d);
            };
            addAndMakeVisible (button);
        }

        exact.onClick = [this] { if (auto* n = selectedNote()) setShift (-n->cents); };
        noCorrection.onClick = [this] { setShift (0); };
        playOriginal.onClick = [this] { playNote (state.selected, false); };
        playCorrected.onClick = [this] { playNote (state.selected, true); };
        autoGo.onClick = [this] { runAuto(); };
        resetAll.onClick = [this] { resetShifts(); };
        playAll.onClick = [this] { togglePlayAll(); };
        apply.onClick = [this] { applyToFile(); };

        for (auto* b : { &exact, &noCorrection, &playOriginal, &playCorrected, &autoGo, &resetAll, &playAll, &apply })
            addAndMakeVisible (b);

        apply.setColour (juce::TextButton::buttonColourId, colours::accent);
        apply.setColour (juce::TextButton::textColourOffId, colours::onAccent);
        autoGo.setColour (juce::TextButton::buttonColourId, colours::accent.withAlpha (0.15f));

        graph.onNoteClicked = [this] (int i) { playNote (i, state.listenCorrected); };
        graph.onNoteDragged = [this] (int i, int cents)
        {
            if (state.selected != i)
                return;
            setShift (cents);
        };
        graph.onNoteDropped = [this] (int i) { playNote (i, true); };
        graph.onTimeClicked = [this] (double t) { playFrom (t); };
        addAndMakeVisible (graph);
        addAndMakeVisible (tuner);

        list.setModel (this);
        list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, colours::surface);
        list.setColour (juce::ListBox::outlineColourId, colours::line);
        list.setOutlineThickness (1);
        list.setHeaderComponent (std::make_unique<HeaderRow>());
        addAndMakeVisible (list);

        updateEditor();
        startTimerHz (30);

        // Up / Down and Space belong to the panel (zoom, play), so the
        // buttons, sliders and the list don't take the keyboard.
        setWantsKeyboardFocus (true);
        list.setWantsKeyboardFocus (false);
        keepKeysHere (*this);
    }

    // Runs the detector the first time the tab is shown and after "Analyse again".
    void ensureRun()
    {
        if (! state.track.has_value() && ! running)
            run();

        if (isShowing())
            grabKeyboardFocus();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            graph.zoom (key == juce::KeyPress::upKey);
            return true;
        }

        // Left / Right: the note before / after the chosen one, played like a
        // click on it (the first arrow starts at either end).
        if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
        {
            const auto count = state.track.has_value() ? (int) state.track->notes.size() : 0;

            if (count > 0)
            {
                const auto step = key == juce::KeyPress::rightKey ? 1 : -1;
                const auto next = juce::isPositiveAndBelow (state.selected, count) ? juce::jlimit (0, count - 1, state.selected + step)
                                                                                   : (step > 0 ? 0 : count - 1);
                playNote (next, state.listenCorrected);
            }

            return true;
        }

        if (key == juce::KeyPress::spaceKey)
        {
            if (model.playing != Model::Playing::nothing)
                model.stop();
            else
                playFrom (graph.playStart());

            return true;
        }

        return false;
    }

    void reset()
    {
        graph.resetView();
        state.track.reset();
        state.corrected.reset();
        state.selected = -1;
        refresh();
    }

    void refreshKey()   { refresh(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::bg);
        paintCard (g, topCard);
        paintCard (g, editCard);
        paintCard (g, autoCard);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        topCard = area.removeFromTop (std::max (300, area.getHeight() / 2 + 40));
        area.removeFromTop (10);

        auto top = topCard.reduced (14);
        auto row = top.removeFromTop (26);
        title.setBounds (row.removeFromLeft (360));
        tuner.setBounds (row.removeFromRight (300).withHeight (48));
        top.removeFromTop (4);

        row = top.removeFromTop (28);
        sourceLabel.setBounds (row.removeFromLeft (70));
        source.setBounds (row.removeFromLeft (300));
        row.removeFromLeft (12);
        tolLabel.setBounds (row.removeFromLeft (72));
        tolerance.setBounds (row.removeFromLeft (170));
        row.removeFromLeft (12);
        onlyProblems.setBounds (row.removeFromLeft (150));
        top.removeFromTop (4);
        row = top.removeFromTop (26);
        referenceLabel.setBounds (row.removeFromLeft (92));
        reference.setBounds (row.removeFromLeft (330));
        top.removeFromTop (8);

        summary.setBounds (top.removeFromTop (22));
        top.removeFromTop (4);
        auto split = top;
        graph.setBounds (split.removeFromLeft (split.getWidth() * 11 / 20));
        split.removeFromLeft (10);
        list.setBounds (split);

        auto bottom = area;
        editCard = bottom.removeFromLeft (bottom.getWidth() / 2 - 5);
        bottom.removeFromLeft (10);
        autoCard = bottom;

        auto e = editCard.reduced (14);
        editInfo.setBounds (e.removeFromTop (22));
        slider.setBounds (e.removeFromTop (28));
        row = e.removeFromTop (28);

        for (auto* b : { &minus10, &minus1, &plus1, &plus10 })
            b->setBounds (row.removeFromLeft (52).reduced (2));

        row.removeFromLeft (6);
        exact.setBounds (row.removeFromLeft (150).reduced (2));
        noCorrection.setBounds (row.removeFromLeft (110).reduced (2));
        e.removeFromTop (4);
        row = e.removeFromTop (28);
        playOriginal.setBounds (row.removeFromLeft (110).reduced (2));
        playCorrected.setBounds (row.removeFromLeft (120).reduced (2));
        row.removeFromLeft (8);
        editResult.setBounds (row);
        e.removeFromTop (4);
        help.setBounds (e);

        auto a = autoCard.reduced (14);
        autoTitle.setBounds (a.removeFromTop (22));
        row = a.removeFromTop (28);
        strengthLabel.setBounds (row.removeFromLeft (84));
        strength.setBounds (row.removeFromLeft (190));
        a.removeFromTop (4);
        row = a.removeFromTop (28);
        scopeLabel.setBounds (row.removeFromLeft (84));
        scope.setBounds (row.removeFromLeft (230));
        a.removeFromTop (4);
        snapToKey.setBounds (a.removeFromTop (28).removeFromLeft (360));
        a.removeFromTop (6);
        row = a.removeFromTop (30);
        autoGo.setBounds (row.removeFromLeft (180).reduced (2));
        resetAll.setBounds (row.removeFromLeft (100).reduced (2));
        a.removeFromTop (10);
        row = a.removeFromTop (30);
        listenLabel.setBounds (row.removeFromLeft (40));
        listen.setBounds (row.removeFromLeft (190));
        row.removeFromLeft (6);
        playAll.setBounds (row.removeFromLeft (80).reduced (2));
        a.removeFromTop (6);
        row = a.removeFromTop (32);
        apply.setBounds (row.removeFromLeft (180).reduced (2));
        row.removeFromLeft (6);
        status.setBounds (row);
    }

private:
    struct HeaderRow final : public juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            g.fillAll (colours::bg);
            g.setColour (colours::muted);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            drawColumns (g, getLocalBounds(), { "Time", "Note", "Hz", "Off by", "Correction", "Status" });
        }
    };

    static void drawColumns (juce::Graphics& g, juce::Rectangle<int> r, const juce::StringArray& texts,
                             const std::array<juce::Colour, 6>* colourPerColumn = nullptr)
    {
        static const float widths[] { 0.14f, 0.12f, 0.12f, 0.13f, 0.22f, 0.27f };
        auto x = (float) r.getX() + 6.0f;
        const auto total = (float) r.getWidth() - 12.0f;

        for (int i = 0; i < texts.size() && i < 6; ++i)
        {
            const auto w = total * widths[i];

            if (colourPerColumn != nullptr)
                g.setColour ((*colourPerColumn)[(size_t) i]);

            g.drawFittedText (texts[i], juce::Rectangle<float> (x, (float) r.getY(), w - 4.0f, (float) r.getHeight()).toNearestInt(),
                              juce::Justification::centredLeft, 1, 0.75f);
            x += w;
        }
    }

    std::vector<int> visibleRows() const
    {
        std::vector<int> result;

        if (! state.track.has_value())
            return result;

        for (size_t i = 0; i < state.track->notes.size(); ++i)
        {
            const auto& s = state.track->notes[i];
            const auto [note, cents] = s.corrected();

            if (! state.onlyProblems || s.shift != 0 || tuneStatus (cents, state.tolerance) != TuneStatus::ok
                || ! isInKey (note, model.keyTonic, model.minor))
                result.push_back ((int) i);
        }

        return result;
    }

    int getNumRows() override  { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
    {
        if (! juce::isPositiveAndBelow (row, (int) rows.size()) || ! state.track.has_value())
            return;

        const auto index = rows[(size_t) row];
        const auto& s = state.track->notes[(size_t) index];
        const auto [note, cents] = s.corrected();
        const auto before = tuneStatus (s.cents, state.tolerance), after = tuneStatus (cents, state.tolerance);
        const auto flats = keyUsesFlats (model.keyTonic, model.minor);
        const auto inKey = isInKey (note, model.keyTonic, model.minor);

        if (index == state.selected)
            g.fillAll (colours::accent.withAlpha (0.12f));

        juce::String label = s.shift != 0 ? (after == TuneStatus::ok ? "corrected" : "still off")
                                          : (after == TuneStatus::ok ? "in tune" : s.cents > 0 ? "too high" : "too low");

        if (! inKey)
            label << ", outside the key";

        const auto correction = s.shift != 0 ? formatCents (s.shift) + juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x92 "))
                                                   + (note != s.note ? midiNoteName (note, flats) + " " : juce::String())
                                                   + formatCents (cents)
                                             : juce::String ("-");

        const std::array<juce::Colour, 6> columnColours { colours::ink, colours::ink, colours::muted, statusColour (before),
                                                          s.shift != 0 ? statusColour (after) : colours::muted,
                                                          ! inKey ? colours::bad : statusColour (after) };
        g.setFont (juce::FontOptions (13.0f));
        drawColumns (g, { 0, 0, width, height },
                     { formatTime (s.start), midiNoteName (s.note, flats), juce::String (s.hz, 1), formatCents (s.cents), correction, label },
                     &columnColours);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, (int) rows.size()))
            playNote (rows[(size_t) row], state.listenCorrected);
    }

    DetectedNote* selectedNote()
    {
        if (! state.track.has_value() || ! juce::isPositiveAndBelow (state.selected, (int) state.track->notes.size()))
            return nullptr;

        return &state.track->notes[(size_t) state.selected];
    }

    void run()
    {
        if (model.host.runJob == nullptr || running)
            return;

        model.stop();
        running = true;
        state.track.reset();
        state.corrected.reset();
        state.selected = -1;
        summary.setText ("Detecting the notes...", juce::dontSendNotification);
        refresh();

        auto result = std::make_shared<std::optional<NoteTrack>>();
        const auto noteSource = state.source;
        const auto referenceCents = this->referenceCents();
        updateReferenceLabels();
        auto audio = std::make_shared<juce::AudioBuffer<float>> (model.source.audio);
        const auto rate = model.source.sampleRate;
        juce::Component::SafePointer<DetectorPanel> safeThis (this);

        model.host.runJob ("Detecting the notes...",
                           [result, audio, rate, noteSource, referenceCents] (std::function<bool (float)> progress)
                           {
                               *result = detectNotes (*audio, { 0, audio->getNumSamples() }, rate, noteSource, progress, referenceCents);
                               return result->has_value();
                           },
                           [safeThis, result] (bool finished)
                           {
                               if (auto* p = safeThis.getComponent())
                               {
                                   p->running = false;

                                   if (finished && result->has_value())
                                       p->state.track = std::move (**result);

                                   p->refresh();
                               }
                           });
    }

    // The song's tuning from the key analysis (0 when there is none or A = 440 Hz is chosen).
    int referenceCents() const
    {
        return state.songReference && model.analysis.has_value() ? model.analysis->tuningCents : 0;
    }

    void updateReferenceLabels()
    {
        const auto cents = model.analysis.has_value() ? model.analysis->tuningCents : 0;
        reference.setLabels ({ cents == 0 ? juce::String ("The song (A = 440 Hz)")
                                          : "The song (A = " + juce::String (440.0 * std::pow (2.0, cents / 1200.0), 1) + " Hz, "
                                                + formatCents (cents) + ")",
                               "Standard A = 440 Hz" });
        reference.setSelected (state.songReference ? 0 : 1);
    }

    void refresh()
    {
        updateReferenceLabels();
        rows = visibleRows();
        list.updateContent();
        list.repaint();
        graph.repaint();
        updateSummary();
        updateEditor();
        updateStatus();
    }

    void updateSummary()
    {
        if (running)
            return;

        if (! state.track.has_value())
        {
            summary.setText ({}, juce::dontSendNotification);
            return;
        }

        const auto& notes = state.track->notes;

        double unclear = 0.0;

        for (auto& r : state.track->unclear)
            unclear += r.getLength();

        // two notes at once (or a chord in the band): the tracker reads one note at a time
        const auto unclearText = unclear < 0.3 ? juce::String()
                               : juce::String (unclear, 1) + " s with no single clear note (two notes at once?), skipped";
        const auto humText = state.track->humHz > 0.0 ? juce::String (state.track->humHz, 1) + " Hz hum ignored" : juce::String();

        if (notes.empty())
        {
            auto text = juce::String (state.source == NoteSource::bass ? "No clear notes in the bass." : "No clear notes in this range.");

            if (unclear >= 0.3)
                text << " " << juce::String (unclear, 1) << " s sound like two or more notes at once: the detector reads one note at a time.";
            else if (state.source == NoteSource::bass)
                text << " Try \"Melody\".";

            summary.setText (text, juce::dontSendNotification);
            return;
        }

        double total = 0.0;
        int off = 0, outside = 0, fixed = 0;
        std::vector<int> centsAfter;

        for (auto& n : notes)
        {
            const auto [note, cents] = n.corrected();
            total += n.end - n.start;
            centsAfter.push_back (cents);

            if (tuneStatus (cents, state.tolerance) != TuneStatus::ok)  ++off;
            if (! isInKey (note, model.keyTonic, model.minor))          ++outside;
            if (n.shift != 0)                                           ++fixed;
        }

        std::sort (centsAfter.begin(), centsAfter.end());
        const auto median = centsAfter[centsAfter.size() / 2];

        auto text = juce::String ((int) notes.size()) + " notes, " + juce::String (total, 1) + " s   |   ";

        if (fixed > 0)
            text << fixed << " corrected   |   ";

        text << off << juce::String (juce::CharPointer_UTF8 (" outside \xc2\xb1")) << state.tolerance << juce::String (juce::CharPointer_UTF8 ("\xc2\xa2   |   "))
             << outside << " outside " << keyName (model.keyTonic, model.minor) << "   |   median " << formatCents (median);

        if (std::abs (median) > state.tolerance && fixed == 0)
            text << "   |   the whole sample is " << (median > 0 ? "high" : "low") << ": tune the bass by " << formatCents (median);

        for (auto& extra : { unclearText, humText })
            if (extra.isNotEmpty())
                text << "   |   " << extra;

        summary.setText (text, juce::dontSendNotification);
    }

    void updateEditor()
    {
        const auto* n = selectedNote();
        const auto on = n != nullptr && ! running;

        for (auto* c : { (juce::Component*) &slider, (juce::Component*) &minus10, (juce::Component*) &minus1, (juce::Component*) &plus1,
                         (juce::Component*) &plus10, (juce::Component*) &exact, (juce::Component*) &noCorrection,
                         (juce::Component*) &playOriginal, (juce::Component*) &playCorrected })
            c->setEnabled (on);

        if (! on)
        {
            editInfo.setText ("Manual correction: pick a note in the graph or the list", juce::dontSendNotification);
            editResult.setText ({}, juce::dontSendNotification);
            slider.setValue (0.0, juce::dontSendNotification);
            exact.setButtonText ("Exactly on the note");
            return;
        }

        const auto flats = keyUsesFlats (model.keyTonic, model.minor);
        const auto [note, cents] = n->corrected();
        editInfo.setText (formatTime (n->start) + "  -  " + midiNoteName (n->note, flats) + "  -  measured " + formatCents (n->cents),
                          juce::dontSendNotification);
        // Fine (one semitone each way) unless the note was dragged further.
        const auto reach = std::abs (n->shift) > 100 ? (double) PitchGraph::maxShiftCents : 100.0;
        slider.setRange (-reach, reach, 1.0);
        slider.setValue (n->shift, juce::dontSendNotification);
        editResult.setText ("after correction: " + midiNoteName (note, flats) + " " + formatCents (cents), juce::dontSendNotification);
        editResult.setColour (juce::Label::textColourId, statusColour (tuneStatus (cents, state.tolerance)));
        exact.setButtonText (n->cents != 0 ? "Exactly on the note (" + formatCents (-n->cents) + ")" : juce::String ("Exactly on the note"));
    }

    void updateStatus()
    {
        int count = 0;

        if (state.track.has_value())
            for (auto& n : state.track->notes)
                if (n.shift != 0)
                    ++count;

        apply.setEnabled (count > 0 && ! running);
        status.setText (count == 0 ? juce::String ("No note corrected yet.")
                                   : juce::String (count) + (count == 1 ? " note corrected" : " notes corrected") + ", not in the file yet",
                        juce::dontSendNotification);
        listen.setSelected (state.listenCorrected ? 1 : 0);
    }

    void setShift (int cents)
    {
        auto* n = selectedNote();

        if (n == nullptr)
            return;

        cents = juce::jlimit (-PitchGraph::maxShiftCents, PitchGraph::maxShiftCents, cents);

        if (cents == n->shift)
        {
            updateEditor();
            return;
        }

        n->shift = cents;
        state.corrected.reset();
        refresh();
    }

    const juce::AudioBuffer<float>& audioFor (bool corrected)
    {
        if (! corrected || ! state.hasShifts())
            return model.source.audio;

        if (! state.corrected.has_value())
            state.corrected = renderCorrection (model.source.audio, { 0, model.source.audio.getNumSamples() },
                                                model.source.sampleRate, state.track->notes, state.source);

        return *state.corrected;
    }

    void playNote (int index, bool corrected)
    {
        if (! state.track.has_value() || ! juce::isPositiveAndBelow (index, (int) state.track->notes.size()))
            return;

        state.selected = index;
        state.listenCorrected = corrected && state.hasShifts();
        refresh();

        const auto pos = std::find (rows.begin(), rows.end(), index);

        if (pos != rows.end())
            list.scrollToEnsureRowIsOnscreen ((int) (pos - rows.begin()));

        const auto& n = state.track->notes[(size_t) index];
        const auto rate = model.source.sampleRate;
        const auto start = std::max (0.0, n.start - 0.04);
        model.stop();
        model.play (slice (audioFor (state.listenCorrected), (int) (start * rate), (int) ((n.end + 0.06) * rate)), {}, false,
                    Model::Playing::audio, start);
    }

    void playFrom (double t)
    {
        const auto rate = model.source.sampleRate;
        model.stop();
        model.play (slice (audioFor (state.listenCorrected), (int) (t * rate), model.source.audio.getNumSamples()), {}, false,
                    Model::Playing::audio, t);
    }

    void togglePlayAll()
    {
        if (model.playing == Model::Playing::audio)
        {
            model.stop();
            return;
        }

        playFrom (0.0);
    }

    void setListenCorrected (bool corrected)
    {
        state.listenCorrected = corrected && state.hasShifts();
        listen.setSelected (state.listenCorrected ? 1 : 0);

        // Switch what is playing without losing the place.
        if (model.playing == Model::Playing::audio)
            if (const auto t = model.playSeconds())
                playFrom (*t);
    }

    void runAuto()
    {
        if (! state.track.has_value() || state.track->notes.empty())
            return;

        const auto changed = autoCorrect (state.track->notes, state.autoSettings, state.tolerance, model.keyTonic, model.minor);

        if (changed == 0)
        {
            status.setText (! state.autoSettings.allNotes
                                ? juce::String (juce::CharPointer_UTF8 ("Nothing to correct: every note is already within \xc2\xb1"))
                                      + juce::String (state.tolerance) + juce::String (juce::CharPointer_UTF8 ("\xc2\xa2. Choose \"All\" or a tighter tolerance."))
                                : juce::String ("Nothing to correct: the notes are already exactly on pitch."),
                            juce::dontSendNotification);
            return;
        }

        state.corrected.reset();
        state.listenCorrected = true;
        refresh();
    }

    void resetShifts()
    {
        if (! state.track.has_value())
            return;

        for (auto& n : state.track->notes)
            n.shift = 0;

        state.corrected.reset();
        state.listenCorrected = false;
        refresh();
    }

    void applyToFile()
    {
        if (! state.hasShifts() || model.source.applyToFile == nullptr)
            return;

        model.stop();
        const auto corrected = audioFor (true);

        if (! model.source.applyToFile (corrected))
        {
            status.setText ("The file is closed: can't apply.", juce::dontSendNotification);
            return;
        }

        // The file now holds the corrected notes: measure them again from there.
        model.source.audio = corrected;
        state.listenCorrected = false;
        run();
        status.setText ("Applied to the file (Ctrl+Z undoes it).", juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const auto t = model.playing == Model::Playing::audio ? model.playSeconds() : std::nullopt;

        if (t.has_value() && model.host.playPosition != nullptr && model.host.playPosition() >= model.playLength)
        {
            model.playing = Model::Playing::nothing;
            graph.setPlayhead ({});
            tuner.set ({}, state.tolerance, false);
            return;
        }

        graph.setPlayhead (t);

        if (! t.has_value() || ! state.track.has_value() || state.track->frames.empty())
        {
            if (lastTunerHadValue)
                tuner.set ({}, state.tolerance, false);

            lastTunerHadValue = false;
            return;
        }

        // The live note: the frame under the playhead, else the note there.
        const auto& frames = state.track->frames;
        const auto i = juce::jlimit (0, (int) frames.size() - 1,
                                     (int) std::lround ((*t - frames.front().time) / state.track->hopSeconds));
        std::optional<double> midi = frames[(size_t) i].midi;
        const DetectedNote* note = nullptr;

        for (auto& n : state.track->notes)
            if (*t >= n.start && *t <= n.end)
                note = &n;

        if (! midi.has_value() && note != nullptr)
            midi = note->note + note->cents / 100.0;

        if (midi.has_value() && note != nullptr && state.listenCorrected)
            *midi += note->shift / 100.0;

        std::optional<std::pair<int, int>> shown;

        if (midi.has_value())
        {
            const auto m = (int) std::lround (*midi);
            shown = std::pair { m, (int) std::lround ((*midi - m) * 100.0) };
        }

        tuner.set (shown, state.tolerance, keyUsesFlats (model.keyTonic, model.minor));
        lastTunerHadValue = shown.has_value();
    }

    Model& model;
    DetectorState state;
    bool running = false, lastTunerHadValue = false;
    std::vector<int> rows;

    juce::Label title, sourceLabel, tolLabel, referenceLabel, summary, status, editInfo, editResult, autoTitle, strengthLabel, scopeLabel, help, listenLabel;
    Segmented source { { "Bass (28-300 Hz)", "Melody (55-1100 Hz)" } };
    Segmented reference { { "The song", "Standard A = 440 Hz" } };
    Segmented tolerance { { juce::String (juce::CharPointer_UTF8 ("\xc2\xb1" "5\xc2\xa2")),
                            juce::String (juce::CharPointer_UTF8 ("\xc2\xb1" "10\xc2\xa2")),
                            juce::String (juce::CharPointer_UTF8 ("\xc2\xb1" "20\xc2\xa2")) } };
    Segmented strength { { "50%", "75%", "100%" } };
    Segmented scope { { "Only the off ones", "All" } };
    Segmented listen { { "Original", "Corrected" } };
    juce::TextButton onlyProblems { "Only problem notes" }, snapToKey { "Also move notes outside the key onto the scale" };
    juce::Slider slider;
    juce::TextButton minus10 { juce::CharPointer_UTF8 ("\xe2\x88\x92" "10\xc2\xa2") }, minus1 { juce::CharPointer_UTF8 ("\xe2\x88\x92" "1\xc2\xa2") },
                     plus1 { juce::CharPointer_UTF8 ("+1\xc2\xa2") }, plus10 { juce::CharPointer_UTF8 ("+10\xc2\xa2") };
    juce::TextButton exact { "Exactly on the note" }, noCorrection { "No correction" };
    juce::TextButton playOriginal { juce::CharPointer_UTF8 ("\xe2\x96\xb6 Original") }, playCorrected { juce::CharPointer_UTF8 ("\xe2\x96\xb6 Corrected") };
    juce::TextButton autoGo { "Correct automatically" }, resetAll { "Reset all" };
    juce::TextButton playAll { juce::CharPointer_UTF8 ("\xe2\x96\xb6 / \xe2\x96\xa0") }, apply { "Apply to file" };
    PitchGraph graph;
    Tuner tuner;
    juce::ListBox list;
    juce::Rectangle<int> topCard, editCard, autoCard;
};

//==============================================================================
// A piano roll of one variant (big with note names, or small on its button).
class RollView final : public juce::Component
{
public:
    RollView (Model& m, bool bigIn) : model (m), big (bigIn) { setInterceptsMouseClicks (false, false); }

    void setNotes (std::vector<bass::Note> n, int barsIn)
    {
        notes = std::move (n);
        bars = barsIn;
        repaint();
    }

    void setCurrent (int index)
    {
        if (index != current)
        {
            current = index;
            repaint();
        }
    }

    void setPlayhead (std::optional<double> fraction)
    {
        if (fraction != playhead)
        {
            playhead = fraction;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto w = (float) getWidth(), h = (float) getHeight();
        g.setColour (colours::bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        const auto total = bars * 16;
        int lo = 99, hi = 0;

        for (auto& n : notes)
        {
            lo = std::min (lo, n.pitch);
            hi = std::max (hi, n.pitch);
        }

        lo -= 2;
        hi += 2;
        const auto range = std::max (8, hi - lo + 1);
        const auto left = big ? 34.0f : 0.0f;
        auto X = [&] (double s) { return (float) (left + s / total * (w - left)); };
        const auto rowHeight = h / (float) range;
        auto Y = [&] (int p) { return h - (float) (p - lo + 1) * rowHeight; };

        if (big)
        {
            const auto flats = keyUsesFlats (model.keyTonic, model.minor);
            g.setFont (juce::FontOptions (10.0f));

            for (int p = lo; p < lo + range; ++p)
            {
                const auto pc = ((p % 12) + 12) % 12;

                if (isInKey (pc, model.keyTonic, model.minor))
                {
                    g.setColour (colours::surface);
                    g.fillRect (left, Y (p), w - left, rowHeight);
                }

                if (pc == model.keyTonic || rowHeight > 11.0f)
                {
                    g.setColour (pc == model.keyTonic ? colours::bass : colours::muted);
                    g.drawText (midiNoteName (p, flats), juce::Rectangle<float> (2.0f, Y (p), left - 2.0f, rowHeight),
                                juce::Justification::centredLeft);
                }
            }
        }

        for (int s = 0; s <= total; s += 4)
        {
            g.setColour (colours::line.withAlpha (s % 16 == 0 ? 1.0f : 0.45f));
            g.fillRect (std::round (X (s)), 0.0f, 1.0f, h);
        }

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            const auto x = X (n.start), ww = std::max (2.0f, X (n.start + n.length) - x - 1.0f);
            const auto y = Y (n.pitch) + 0.5f, hh = std::max (2.0f, rowHeight - 1.0f);
            g.setColour (colours::bass.withAlpha (0.35f + 0.65f * (float) n.velocity / 127.0f));
            g.fillRoundedRectangle (x, y, ww, hh, std::min (3.0f, hh / 2.0f));

            if ((int) i == current)
            {
                g.setColour (colours::ink);
                g.drawRoundedRectangle (x - 1.5f, y - 1.5f, ww + 3.0f, hh + 3.0f, std::min (4.0f, hh / 2.0f + 1.5f), 2.0f);
            }

            if (n.glide && i > 0 && big)
            {
                g.setColour (colours::bass.withAlpha (0.8f));
                g.drawLine (x - 4.0f, Y (notes[i - 1].pitch) + rowHeight / 2.0f, x + 4.0f, y + hh / 2.0f, 1.5f);
            }
        }

        if (playhead.has_value())
        {
            g.setColour (colours::accent);
            g.fillRect (X (*playhead * total) - 1.0f, 0.0f, 2.0f, h);
        }
    }

private:
    Model& model;
    bool big;
    std::vector<bass::Note> notes;
    int bars = 8;
    int current = -1;   // the note chosen with the arrows
    std::optional<double> playhead;
};

// A choosable variant: its name, note count and a small piano roll.
class VariantButton final : public juce::Component
{
public:
    VariantButton (Model& m) : roll (m, false) { addAndMakeVisible (roll); }

    std::function<void()> onClick;

    void set (const juce::String& text, bool isSelected)
    {
        label = text;
        selected = isSelected;
        repaint();
    }

    RollView roll;

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (selected ? colours::accent.withAlpha (0.08f) : colours::surface);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (selected ? colours::accent : colours::line);
        g.drawRoundedRectangle (r, 8.0f, selected ? 2.0f : 1.0f);
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (label, getLocalBounds().reduced (10, 4).removeFromTop (20), juce::Justification::centredLeft);
    }

    void resized() override   { roll.setBounds (getLocalBounds().reduced (8).withTrimmedTop (22)); }
    void mouseDown (const juce::MouseEvent&) override   { if (onClick != nullptr) onClick(); }

private:
    juce::String label;
    bool selected = false;
};

// The chord root of every bar; clicking moves it one scale step.
class BarChips final : public juce::Component
{
public:
    explicit BarChips (Model& m) : model (m) {}

    std::vector<int> roots, thirds;   // thirds: 3 = minor chord, 4 = major
    std::function<void (int)> onBarClicked;

    void paint (juce::Graphics& g) override
    {
        const auto count = (int) roots.size();

        if (count == 0)
            return;

        const auto flats = keyUsesFlats (model.keyTonic, model.minor);
        const auto w = (float) getWidth() / (float) count;

        for (int b = 0; b < count; ++b)
        {
            const auto r = juce::Rectangle<float> (w * (float) b, 0.0f, w, (float) getHeight()).reduced (2.0f);
            const auto tonic = roots[(size_t) b] == model.keyTonic;
            g.setColour (tonic ? colours::bass.withAlpha (0.14f) : colours::surface);
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (tonic ? colours::bass : colours::line);
            g.drawRoundedRectangle (r, 6.0f, 1.0f);
            g.setColour (colours::muted);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (juce::String (b + 1), r.withHeight (14.0f), juce::Justification::centred);
            g.setColour (tonic ? colours::bass : colours::ink);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            const auto minorChord = (size_t) b < thirds.size() && thirds[(size_t) b] == 3;
            g.drawText (noteName (roots[(size_t) b], flats) + (minorChord ? "m" : ""), r.withTrimmedTop (12.0f), juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (roots.empty() || onBarClicked == nullptr)
            return;

        onBarClicked (juce::jlimit (0, (int) roots.size() - 1, e.x * (int) roots.size() / std::max (1, getWidth())));
    }

private:
    Model& model;
};

//==============================================================================
// Tab 3: bass line proposals in MIDI.
class GeneratorPanel final : public juce::Component,
                             private juce::Timer
{
public:
    void spacePlay()   { togglePreview(); }   // Space in the window: the bass preview
    explicit GeneratorPanel (Model& m)
        : model (m), chips (m), roll (m, true)
    {
        styleLabel (title, 16.0f, true);
        title.setText ("MIDI generator - bass line proposals", juce::dontSendNotification);

        for (auto* l : { &title, &styleTitle, &keyTitle, &tempoTitle, &downbeatTitle, &downbeatValue, &downbeatHelp, &lengthTitle,
                         &complexityTitle, &octaveTitle, &toneTitle, &harmonyTitle, &chipsTitle, &startInfo, &sampleVolLabel,
                         &bassVolLabel, &exportStatus, &loopInfo })
            addAndMakeVisible (l);

        for (auto* l : { &styleTitle, &keyTitle, &tempoTitle, &downbeatTitle, &lengthTitle, &complexityTitle, &octaveTitle,
                         &toneTitle, &harmonyTitle, &chipsTitle })
            styleLabel (*l, 12.0f, true, colours::muted);

        styleLabel (downbeatValue, 13.0f, true);
        styleLabel (downbeatHelp, 12.0f, false, colours::muted);
        styleLabel (startInfo, 12.0f, false, colours::muted);
        styleLabel (sampleVolLabel, 12.0f, false, colours::muted);
        styleLabel (bassVolLabel, 12.0f, false, colours::muted);
        styleLabel (exportStatus, 12.0f, false, colours::muted);
        styleLabel (loopInfo, 12.0f, false, colours::muted);

        styleTitle.setText ("Style", juce::dontSendNotification);
        keyTitle.setText ("Key used", juce::dontSendNotification);
        tempoTitle.setText ("Tempo (BPM)", juce::dontSendNotification);
        downbeatTitle.setText ("First beat in the sample", juce::dontSendNotification);
        downbeatHelp.setText ("Where beat 1 of bar 1 falls in the sample. A loop cut on the beat needs 0 ms.", juce::dontSendNotification);
        lengthTitle.setText ("Length", juce::dontSendNotification);
        complexityTitle.setText ("Complexity", juce::dontSendNotification);
        octaveTitle.setText ("Octave", juce::dontSendNotification);
        toneTitle.setText ("Sound", juce::dontSendNotification);
        harmonyTitle.setText ("Harmony", juce::dontSendNotification);
        chipsTitle.setText ("Root note of each bar (click a bar to change it)", juce::dontSendNotification);
        sampleVolLabel.setText ("Sample", juce::dontSendNotification);
        bassVolLabel.setText ("Bass", juce::dontSendNotification);

        const auto& list = bass::styles();

        for (size_t i = 0; i < list.size(); ++i)
        {
            auto* b = styleButtons.add (new juce::TextButton (juce::String (list[i].name) + "\n" + list[i].description));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (101);
            b->setColour (juce::TextButton::buttonOnColourId, colours::accent.withAlpha (0.18f));
            b->setColour (juce::TextButton::textColourOnId, colours::accent);
            b->onClick = [this, i] { if (styleButtons[(int) i]->getToggleState()) { settings.style = (int) i; clearImports(); regenerate(); } };
            addAndMakeVisible (b);
        }

        styleButtons[0]->setToggleState (true, juce::dontSendNotification);

        for (int minor = 1; minor >= 0; --minor)
            for (int t = 0; t < 12; ++t)
                keyBox.addItem (keyLabel (t, minor != 0), t * 2 + minor + 1);

        keyBox.onChange = [this]
        {
            const auto id = keyBox.getSelectedId() - 1;

            if (id < 0)
                return;

            model.keyTonic = id / 2;
            model.minor = (id & 1) != 0;

            if (model.keyChanged != nullptr)
                model.keyChanged();

            recomputeRoots();
        };

        bpmField.setInputRestrictions (7, "0123456789.,");
        bpmField.setJustification (juce::Justification::centred);
        // Its own colours: the MIDI tab is built before it gets the dark look,
        // so the tempo found was written in black on the dark box.
        bpmField.setColour (juce::TextEditor::backgroundColourId, colours::bg);
        bpmField.setColour (juce::TextEditor::textColourId, colours::ink);
        bpmField.onReturnKey = [this] { bpmTyped(); };
        bpmField.onFocusLost = [this] { bpmTyped(); };
        half.onClick = [this] { manualTempo(); setBpm (settings.bpm / 2.0); };
        twice.onClick = [this] { manualTempo(); setBpm (settings.bpm * 2.0); };

        // Auto: the tempo found in the sample; any change by hand (typing,
        // ÷2 / ×2, Tap) switches to Manual, and Auto brings the found one back.
        autoTempoButton.setClickingTogglesState (true);
        autoTempoButton.setToggleState (true, juce::dontSendNotification);
        autoTempoButton.setTooltip ("On: the tempo found in the sample. Off: the one you set.");
        autoTempoButton.onClick = [this]
        {
            if (autoTempoButton.getToggleState())
                useDetectedTempo();
            else
                updateGridUi();
        };

        tapButton.setTooltip ("Tap in time with the sample (4 taps or more): sets the tempo by hand");
        tapButton.onClick = [this] { tap(); };

        beatBack.onClick = [this] { shiftBeat (-1); };
        beatForward.onClick = [this] { shiftBeat (1); };
        msBack.onClick = [this] { setDownbeat (settings.downbeat - 0.01); };
        msForward.onClick = [this] { setDownbeat (settings.downbeat + 0.01); };
        msZero.onClick = [this] { setDownbeat (0.0); };

        length.setSelected (1);
        length.onChange = [this] (int i) { settings.bars = i == 0 ? 4 : i == 1 ? 8 : 16; recomputeRoots(); };
        complexity.setSelected (1);
        complexity.onChange = [this] (int i) { settings.complexity = i + 1; regenerate(); };
        octave.setSelected (1);
        octave.onChange = [this] (int i) { settings.octave = i - 1; regenerate(); };
        harmony.onChange = [this] (int i) { settings.followChords = i == 0; recomputeRoots(); };

        toneBox.addItem ("808 (every style)", 1);
        toneBox.addItem ("By style", 2);
        toneBox.addItem ("Clean sub (sine)", 3);
        toneBox.addItem ("Synth bass (filtered saw)", 4);
        toneBox.addItem ("Reese (DnB)", 5);
        toneBox.addItem ("Pluck / Funk", 6);
        toneBox.setSelectedId (1, juce::dontSendNotification);
        toneBox.onChange = [this]
        {
            static const bass::Tone tones[] { bass::Tone::tr808, bass::Tone::byStyle, bass::Tone::sub, bass::Tone::synth,
                                              bass::Tone::reese, bass::Tone::pluck };
            settings.tone = tones[juce::jlimit (0, 5, toneBox.getSelectedId() - 1)];
            regenerate();
        };

        chips.onBarClicked = [this] (int b)
        {
            roots[(size_t) b] = bass::nextScaleRoot (roots[(size_t) b], model.keyTonic, model.minor);

            if ((size_t) b < thirds.size())
                thirds[(size_t) b] = bass::scaleThird (roots[(size_t) b], model.keyTonic, model.minor);

            regenerate (false);
        };

        fromPlayhead.onClick = [this]
        {
            const auto t = model.source.cursorSeconds != nullptr ? model.source.cursorSeconds() : 0.0;
            const auto bar = 240.0 / settings.bpm;
            settings.startBar = juce::jlimit (0, bass::totalBars (model.duration(), settings.downbeat, settings.bpm) - 1,
                                              (int) std::floor ((t - settings.downbeat) / bar));
            recomputeRoots();
        };

        fromStart.onClick = [this] { settings.startBar = 0; recomputeRoots(); };
        reroll.onClick = [this]
        {
            for (auto& s : settings.seeds)
                s = (juce::uint32) juce::Random::getSystemRandom().nextInt (1000000000);

            clearImports();
            regenerate (false);
        };

        for (int i = 0; i < 3; ++i)
        {
            auto* v = variantButtons.add (new VariantButton (model));
            v->onClick = [this, i] { selected = i; currentNote = -1; refreshVariants(); restartPreviewIfPlaying(); };
            addAndMakeVisible (v);
        }

        previewButton.onClick = [this] { togglePreview(); };
        previewButton.setColour (juce::TextButton::buttonColourId, colours::accent);
        previewButton.setColour (juce::TextButton::textColourOffId, colours::onAccent);

        for (auto [slider, value] : { std::pair { &sampleVol, 0.7 }, std::pair { &bassVol, 0.8 } })
        {
            slider->setRange (0.0, 1.0, 0.01);
            slider->setValue (value, juce::dontSendNotification);
            slider->setSliderStyle (juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            slider->onValueChange = [this] { updateGains(); };
            addAndMakeVisible (slider);
        }

        importMidi.setTooltip ("A MIDI file into the chosen variant (A, B or C), played at the sample's tempo");
        importMidi.onClick = [this] { chooseMidiToImport(); };
        saveOne.onClick = [this] { saveMidi (false); };
        saveAll.onClick = [this] { saveMidi (true); };
        bassToNew.onClick = [this] { renderToNewFile (false); };
        mixToNew.onClick = [this] { renderToNewFile (true); };

        for (auto* b : { &autoTempoButton, &tapButton, &half, &twice, &beatBack, &msBack, &msZero, &msForward, &beatForward, &fromPlayhead, &fromStart, &reroll, &importMidi,
                         &previewButton, &saveOne, &saveAll, &bassToNew, &mixToNew })
            addAndMakeVisible (b);

        for (auto* c : { (juce::Component*) &keyBox, (juce::Component*) &bpmField, (juce::Component*) &length,
                         (juce::Component*) &complexity, (juce::Component*) &octave, (juce::Component*) &toneBox,
                         (juce::Component*) &harmony, (juce::Component*) &chips, (juce::Component*) &roll })
            addAndMakeVisible (c);

        loadFromModel();
        startTimerHz (30);

        // Left / Right go from note to note, so the buttons don't take the keyboard.
        setWantsKeyboardFocus (true);
        keepKeysHere (*this);
    }

    void takeKeyboard()
    {
        if (isShowing())
            grabKeyboardFocus();
    }

    void mouseDown (const juce::MouseEvent&) override   { grabKeyboardFocus(); }

    // Left / Right: the note before / after the chosen one, played with the
    // sample under it (the first arrow starts at either end).
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key != juce::KeyPress::leftKey && key != juce::KeyPress::rightKey)
            return false;

        const auto& notes = variants[(size_t) selected];

        if (notes.empty())
            return true;

        const auto step = key == juce::KeyPress::rightKey ? 1 : -1;
        currentNote = currentNote < 0 ? (step > 0 ? 0 : (int) notes.size() - 1)
                                      : juce::jlimit (0, (int) notes.size() - 1, currentNote + step);
        roll.setCurrent (currentNote);
        playNote (notes[(size_t) currentNote]);
        return true;
    }

    // After the analysis: tempo, first beat and key from the audio.
    void loadFromModel()
    {
        stopPreview();
        settings.keyTonic = model.keyTonic;
        settings.minor = model.minor;
        settings.tuningCents = model.analysis.has_value() ? model.analysis->tuningCents : 0;

        if (autoTempoButton.getToggleState())   // a tempo set by hand stays
        {
            settings.bpm = model.tempo.has_value() ? model.tempo->bpm : 120.0;
            settings.downbeat = model.tempo.has_value() ? model.tempo->downbeat : 0.0;
        }
        settings.startBar = 0;

        if (model.tempo.has_value() && model.tempo->loopBars > 0)
        {
            settings.bars = model.tempo->loopBars <= 4 ? 4 : model.tempo->loopBars <= 8 ? 8 : 16;
            length.setSelected (settings.bars == 4 ? 0 : settings.bars == 8 ? 1 : 2);
        }

        keyBox.setSelectedId (settings.keyTonic * 2 + (settings.minor ? 1 : 0) + 1, juce::dontSendNotification);
        recomputeRoots();
    }

    void keyChangedElsewhere()
    {
        keyBox.setSelectedId (model.keyTonic * 2 + (model.minor ? 1 : 0) + 1, juce::dontSendNotification);
        recomputeRoots();
    }

    void stopPreview()
    {
        if (model.playing == Model::Playing::preview)
            model.stop();

        previewPlayer = nullptr;
        previewButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6 Listen with the sample"));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::bg);
        paintCard (g, settingsCard);
        paintCard (g, resultCard);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        settingsCard = area.removeFromTop (266);
        area.removeFromTop (10);
        resultCard = area;

        auto s = settingsCard.reduced (14);
        title.setBounds (s.removeFromTop (24));
        styleTitle.setBounds (s.removeFromTop (16));
        auto styleArea = s.removeFromTop (76);
        const auto perRow = 5;
        const auto bw = styleArea.getWidth() / perRow, bh = styleArea.getHeight() / 2;

        for (int i = 0; i < styleButtons.size(); ++i)
            styleButtons[i]->setBounds (styleArea.getX() + (i % perRow) * bw, styleArea.getY() + (i / perRow) * bh, bw - 4, bh - 4);

        s.removeFromTop (6);
        auto column = [] (juce::Rectangle<int>& row, int width) { auto c = row.removeFromLeft (width); row.removeFromLeft (12); return c; };

        auto row = s.removeFromTop (48);
        auto c = column (row, 230);
        keyTitle.setBounds (c.removeFromTop (16));
        keyBox.setBounds (c.removeFromTop (28));
        c = column (row, 330);
        tempoTitle.setBounds (c.removeFromTop (16));
        auto t = c.removeFromTop (28);
        autoTempoButton.setBounds (t.removeFromLeft (56));
        t.removeFromLeft (4);
        bpmField.setBounds (t.removeFromLeft (72));
        t.removeFromLeft (4);
        half.setBounds (t.removeFromLeft (42));
        t.removeFromLeft (4);
        twice.setBounds (t.removeFromLeft (42));
        t.removeFromLeft (4);
        tapButton.setBounds (t.removeFromLeft (60));
        c = row;
        downbeatTitle.setBounds (c.removeFromTop (16));
        auto d = c.removeFromTop (28);
        downbeatValue.setBounds (d.removeFromLeft (70));

        for (auto* b : { &beatBack, &msBack, &msZero, &msForward, &beatForward })
        {
            b->setBounds (d.removeFromLeft (64));
            d.removeFromLeft (4);
        }

        downbeatHelp.setBounds (s.removeFromTop (18));
        s.removeFromTop (4);

        row = s.removeFromTop (48);
        c = column (row, 200);
        lengthTitle.setBounds (c.removeFromTop (16));
        length.setBounds (c.removeFromTop (28));
        c = column (row, 230);
        complexityTitle.setBounds (c.removeFromTop (16));
        complexity.setBounds (c.removeFromTop (28));
        c = column (row, 190);
        octaveTitle.setBounds (c.removeFromTop (16));
        octave.setBounds (c.removeFromTop (28));
        c = column (row, 200);
        toneTitle.setBounds (c.removeFromTop (16));
        toneBox.setBounds (c.removeFromTop (28));
        c = row;
        harmonyTitle.setBounds (c.removeFromTop (16));
        harmony.setBounds (c.removeFromTop (28).withWidth (std::min (c.getWidth(), 280)));

        auto r = resultCard.reduced (14);
        auto top = r.removeFromTop (16);
        chipsTitle.setBounds (top.removeFromLeft (360));
        loopInfo.setBounds (top);
        auto chipRow = r.removeFromTop (40);
        importMidi.setBounds (chipRow.removeFromRight (110).reduced (2, 6));
        reroll.setBounds (chipRow.removeFromRight (130).reduced (2, 6));
        fromStart.setBounds (chipRow.removeFromRight (90).reduced (2, 6));
        fromPlayhead.setBounds (chipRow.removeFromRight (150).reduced (2, 6));
        chips.setBounds (chipRow);
        auto infoRow = r.removeFromTop (16);
        startInfo.setBounds (infoRow.removeFromLeft (360));
        exportStatus.setBounds (infoRow);
        exportStatus.setJustificationType (juce::Justification::centredRight);
        r.removeFromTop (4);

        auto variantRow = r.removeFromTop (84);
        const auto vw = variantRow.getWidth() / 3;

        for (int i = 0; i < 3; ++i)
            variantButtons[i]->setBounds (variantRow.getX() + i * vw, variantRow.getY(), vw - 8, variantRow.getHeight());

        r.removeFromTop (8);
        auto bottom = r.removeFromBottom (32);
        r.removeFromBottom (6);
        roll.setBounds (r);

        // Preview and export on one row, so the piano roll keeps its height.
        previewButton.setBounds (bottom.removeFromLeft (190).reduced (2));
        bottom.removeFromLeft (6);
        sampleVolLabel.setBounds (bottom.removeFromLeft (48));
        sampleVol.setBounds (bottom.removeFromLeft (100));
        bottom.removeFromLeft (6);
        bassVolLabel.setBounds (bottom.removeFromLeft (34));
        bassVol.setBounds (bottom.removeFromLeft (100));
        bottom.removeFromLeft (10);

        for (auto* b : { &mixToNew, &bassToNew, &saveAll, &saveOne })
            b->setBounds (bottom.removeFromRight (b == &saveOne ? 170 : b == &mixToNew ? 160 : 140).reduced (2));
    }

private:
    void bpmTyped()
    {
        const auto v = bpmField.getText().replaceCharacter (',', '.').getDoubleValue();

        if (v > 0.0 && std::abs (v - settings.bpm) > 0.004)
        {
            manualTempo();
            setBpm (v);
        }
        else
            updateGridUi();
    }

    void manualTempo()
    {
        autoTempoButton.setToggleState (false, juce::dontSendNotification);
    }

    // Auto again: the tempo (and first beat) found in the sample.
    void useDetectedTempo()
    {
        settings.bpm = model.tempo.has_value() ? model.tempo->bpm : 120.0;
        settings.downbeat = model.tempo.has_value() ? model.tempo->downbeat : 0.0;
        recomputeRoots();
    }

    // Tap: the average of the last taps (a pause of 2 s starts over).
    void tap()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (! taps.empty() && now - taps.back() > 2000.0)
            taps.clear();

        taps.push_back (now);

        if (taps.size() > 9)
            taps.erase (taps.begin());

        if (taps.size() < 4)
        {
            tempoTitle.setText ("Tap: " + juce::String ((int) taps.size()) + " of 4...", juce::dontSendNotification);
            return;
        }

        const auto beat = (taps.back() - taps.front()) / (double) (taps.size() - 1) / 1000.0;
        auto bpm = 60.0 / beat;

        if (std::abs (bpm - std::round (bpm)) < 0.5)
            bpm = std::round (bpm);

        manualTempo();
        setBpm (bpm);
    }

    void setBpm (double v)
    {
        settings.bpm = juce::jlimit (40.0, 220.0, std::round (v * 100.0) / 100.0);

        if (settings.downbeat >= 240.0 / settings.bpm)
            settings.downbeat = 0.0;

        recomputeRoots();
    }

    void setDownbeat (double v)
    {
        const auto bar = 240.0 / settings.bpm;
        settings.downbeat = juce::jlimit (0.0, bar - 0.001, v);
        recomputeRoots();
    }

    void shiftBeat (int direction)
    {
        const auto beat = 60.0 / settings.bpm, bar = beat * 4.0;
        auto v = settings.downbeat + direction * beat;
        v = std::fmod (std::fmod (v, bar) + bar, bar);

        if (v > bar - 0.0005)
            v = 0.0;

        setDownbeat (v);
    }

    void updateGridUi()
    {
        bpmField.setText (formatNumber (settings.bpm, 2), false);

        // how sure the found tempo is (a sample without drums may have no clear beat)
        if (! autoTempoButton.getToggleState())
            tempoTitle.setText ("Tempo (BPM)  -  manual (Auto: the one found)", juce::dontSendNotification);
        else if (! model.tempo.has_value())
            tempoTitle.setText ("Tempo (BPM)  -  none found: type it or tap", juce::dontSendNotification);
        else
        {
            const auto c = model.tempo->confidence;
            tempoTitle.setText (juce::String ("Tempo (BPM)  -  ") + (model.tempoFromFile ? "the file's tempo" : c >= 0.55 ? "found, sure" : c >= 0.4 ? "found, probably right"
                                                                                 : "a guess: check it (type it or tap)"),
                                juce::dontSendNotification);
        }

        tempoTitle.setColour (juce::Label::textColourId, autoTempoButton.getToggleState() && model.tempo.has_value()
                                                             && model.tempo->confidence < 0.4 ? colours::warn : colours::muted);
        downbeatValue.setText (juce::String (juce::roundToInt (settings.downbeat * 1000.0)) + " ms", juce::dontSendNotification);

        const auto x = (model.duration() - settings.downbeat) / (240.0 / settings.bpm);
        const auto r = std::round (x);
        loopInfo.setText (r >= 1.0 && std::abs (x - r) < 0.03 ? "the sample is a loop of " + juce::String ((int) r) + ((int) r == 1 ? " bar" : " bars")
                                                              : "the sample is " + juce::String (x, 2) + " bars long",
                          juce::dontSendNotification);
        startInfo.setText ("The line starts at bar " + juce::String (settings.startBar + 1) + " of the sample.", juce::dontSendNotification);
    }

    void recomputeRoots()
    {
        settings.keyTonic = model.keyTonic;
        settings.minor = model.minor;
        roots = bass::computeRoots (settings, model.analysis.has_value() ? &*model.analysis : nullptr, model.duration());
        thirds = bass::computeThirds (settings, model.analysis.has_value() ? &*model.analysis : nullptr, model.duration(), roots);
        regenerate (false);
    }

    void regenerate (bool recompute = true)
    {
        if (recompute && roots.size() != (size_t) settings.bars)
        {
            roots = bass::computeRoots (settings, model.analysis.has_value() ? &*model.analysis : nullptr, model.duration());
            thirds = bass::computeThirds (settings, model.analysis.has_value() ? &*model.analysis : nullptr, model.duration(), roots);
        }

        settings.keyTonic = model.keyTonic;
        settings.minor = model.minor;
        variants = bass::generate (settings, roots, thirds);
        currentNote = -1;

        for (size_t i = 0; i < variants.size(); ++i)   // an imported line stays
            if (importedName[i].isNotEmpty())
                variants[i] = imported[i];

        chips.roots = roots;
        chips.thirds = thirds;
        chips.repaint();
        updateGridUi();
        refreshVariants();
        restartPreviewIfPlaying();
    }

    void refreshVariants()
    {
        static const char* letters[] { "A", "B", "C" };

        for (int i = 0; i < 3; ++i)
        {
            auto* v = variantButtons[i];
            const auto name = importedName[(size_t) i].isNotEmpty() ? "Imported: " + importedName[(size_t) i] : juce::String (bass::characterName (i));
            v->set (juce::String (letters[i]) + "  -  " + name + "   " + juce::String ((int) variants[(size_t) i].size()) + " notes",
                    i == selected);
            v->roll.setNotes (variants[(size_t) i], settings.bars);
        }

        roll.setNotes (variants[(size_t) selected], settings.bars);
        roll.setCurrent (currentNote);
        saveOne.setButtonText (juce::String ("Save MIDI (variant ") + letters[selected] + ")");
    }

    juce::AudioBuffer<float> sampleAudio() const
    {
        return bass::sampleLoop (model.source.audio, { 0, model.source.audio.getNumSamples() }, model.source.sampleRate, settings);
    }

    juce::AudioBuffer<float> bassAudio() const
    {
        return bass::renderBass (variants[(size_t) selected], bass::resolvedTone (settings), settings.bpm, settings.bars,
                                 model.source.sampleRate, settings.tuningCents);
    }

    // One note with the sample under it, at the volumes of the two sliders.
    void playNote (const bass::Note& n)
    {
        stopPreview();
        const auto sixteenth = 15.0 / settings.bpm * model.source.sampleRate;
        auto sample = sampleAudio();
        auto bassLine = bassAudio();
        const auto total = std::min (sample.getNumSamples(), bassLine.getNumSamples());
        const auto from = juce::jlimit (0, total, (int) std::lround (n.start * sixteenth));
        const auto to = juce::jlimit (from, total, (int) std::lround ((n.start + n.length) * sixteenth + 0.08 * model.source.sampleRate));
        juce::AudioBuffer<float> sampleCut (sample.getNumChannels(), to - from), bassCut (bassLine.getNumChannels(), to - from);

        for (int ch = 0; ch < sample.getNumChannels(); ++ch)
            sampleCut.copyFrom (ch, 0, sample, ch, from, to - from);

        for (int ch = 0; ch < bassLine.getNumChannels(); ++ch)
            bassCut.copyFrom (ch, 0, bassLine, ch, from, to - from);

        LayeredPlayer* created = nullptr;
        model.play (std::move (sampleCut), std::move (bassCut), false, Model::Playing::tone, 0.0, &created);

        if (created != nullptr)
        {
            created->mainGain = (float) sampleVol.getValue();
            created->layerGain = (float) bassVol.getValue();
        }

        const auto flats = keyUsesFlats (model.keyTonic, model.minor);
        exportStatus.setText ("Note " + juce::String (currentNote + 1) + " of " + juce::String ((int) variants[(size_t) selected].size()) + ": "
                                  + midiNoteName (n.pitch, flats) + ", bar " + juce::String (n.start / 16 + 1) + " beat "
                                  + juce::String ((n.start % 16) / 4 + 1),
                              juce::dontSendNotification);
    }

    void togglePreview()
    {
        if (previewPlayer != nullptr && model.playing == Model::Playing::preview)
        {
            stopPreview();
            return;
        }

        startPreview (0);
    }

    void startPreview (juce::int64 position)
    {
        model.stop();
        LayeredPlayer* created = nullptr;
        model.play (sampleAudio(), bassAudio(), true, Model::Playing::preview, 0.0, &created);
        previewPlayer = created;

        if (previewPlayer != nullptr)
        {
            previewPlayer->setNextReadPosition (position);
            updateGains();
        }

        previewButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xa0 Stop"));
    }

    void restartPreviewIfPlaying()
    {
        if (previewPlayer == nullptr || model.playing != Model::Playing::preview)
            return;

        const auto loopLength = std::max ((juce::int64) 1, previewPlayer->getLength());
        startPreview (previewPlayer->getNextReadPosition() % loopLength);
    }

    void updateGains()
    {
        if (previewPlayer != nullptr && model.playing == Model::Playing::preview)
        {
            previewPlayer->mainGain = (float) sampleVol.getValue();
            previewPlayer->layerGain = (float) bassVol.getValue();
        }
    }

    void timerCallback() override
    {
        if (previewPlayer == nullptr || model.playing != Model::Playing::preview)
        {
            if (previewPlayer != nullptr)
                stopPreview();

            roll.setPlayhead ({});

            for (auto* v : variantButtons)
                v->roll.setPlayhead ({});

            return;
        }

        const auto loopLength = std::max ((juce::int64) 1, previewPlayer->getLength());
        const auto fraction = (double) (previewPlayer->getNextReadPosition() % loopLength) / (double) loopLength;
        roll.setPlayhead (fraction);

        for (int i = 0; i < 3; ++i)
            variantButtons[i]->roll.setPlayhead (i == selected ? std::optional<double> (fraction) : std::nullopt);
    }

    void clearImports()
    {
        for (auto& n : importedName)
            n.clear();
    }

    // Import MIDI: the notes go into the chosen variant and play at the
    // sample's tempo (the file's own tempo is ignored); the length becomes
    // 4, 8 or 16 bars, enough for the notes.
    void chooseMidiToImport()
    {
        if (model.host.chooseOpenFile == nullptr)
            return;

        juce::Component::SafePointer<GeneratorPanel> safeThis (this);
        model.host.chooseOpenFile ("Import MIDI", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.mid;*.midi",
                                   [safeThis] (juce::File file)
        {
            if (auto* p = safeThis.getComponent(); p != nullptr && file != juce::File())
                p->importMidiFile (file);
        });
    }

    void importMidiFile (const juce::File& file)
    {
        juce::FileInputStream in (file);
        juce::MidiFile midi;
        int bars = 0;
        std::vector<bass::Note> notes;

        if (in.openedOk() && midi.readFrom (in))
            notes = bass::notesFromMidi (midi, bars);

        if (notes.empty())
        {
            exportStatus.setText ("No notes found in " + file.getFileName(), juce::dontSendNotification);
            return;
        }

        imported[(size_t) selected] = std::move (notes);
        importedName[(size_t) selected] = file.getFileNameWithoutExtension();
        settings.bars = bars <= 4 ? 4 : bars <= 8 ? 8 : 16;
        length.setSelected (settings.bars == 4 ? 0 : settings.bars == 8 ? 1 : 2);
        recomputeRoots();
        exportStatus.setText ("Imported " + file.getFileName() + ": " + juce::String (bars) + " bars at " + formatNumber (settings.bpm, 2) + " BPM"
                                  + (bars > 16 ? juce::String (" (only the first 16 play)") : juce::String()),
                              juce::dontSendNotification);
    }

    void saveMidi (bool all)
    {
        if (model.host.chooseSaveFile == nullptr)
            return;

        static const char* letters[] { "A", "B", "C" };
        const auto base = bass::fileTag (settings);
        const auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                   .getChildFile (all ? base + ".mid" : base + "_" + letters[selected] + ".mid");
        const auto snapshot = variants;
        const auto chosen = selected;
        const auto s = settings;
        juce::Component::SafePointer<GeneratorPanel> safeThis (this);

        model.host.chooseSaveFile (all ? "Save the 3 MIDI variants (A, B, C are added to the name)" : "Save MIDI", suggested, "*.mid",
                                   [safeThis, snapshot, chosen, all, s] (juce::File file)
        {
            if (file == juce::File())
                return;

            const auto glide = bass::styleGlides (s.style);
            const auto styleName = juce::String (bass::styles()[(size_t) s.style].name);
            juce::StringArray written;
            bool ok = true;

            for (int i = 0; i < 3; ++i)
            {
                if (! all && i != chosen)
                    continue;

                auto target = all ? file.getSiblingFile (file.getFileNameWithoutExtension() + "_" + letters[i] + ".mid")
                                  : file.withFileExtension ("mid");
                const auto data = bass::midiFile (snapshot[(size_t) i], s.bpm, "Bass " + styleName + " " + letters[i], glide, s.tuningCents);
                ok = target.replaceWithData (data.getData(), data.getSize()) && ok;
                written.add (target.getFileName());
            }

            if (auto* p = safeThis.getComponent())
                p->exportStatus.setText (ok ? "Saved: " + written.joinIntoString (", ")
                                                  + (s.tuningCents != 0 ? " (pitch bend " + formatCents (s.tuningCents) + ", in tune with the sample)"
                                                                        : juce::String())
                                            : juce::String ("Couldn't save the file."),
                                         juce::dontSendNotification);
        });
    }

    void renderToNewFile (bool withSample)
    {
        if (model.host.openAsNewFile == nullptr)
            return;

        static const char* letters[] { "A", "B", "C" };
        auto bassBuffer = bassAudio();
        juce::AudioBuffer<float> out;

        if (withSample)
        {
            auto sample = sampleAudio();
            const auto channels = std::max (sample.getNumChannels(), 2);
            out.setSize (channels, std::max (sample.getNumSamples(), bassBuffer.getNumSamples()));
            out.clear();

            for (int ch = 0; ch < channels; ++ch)
            {
                if (sample.getNumChannels() > 0)
                    out.addFrom (ch, 0, sample, std::min (ch, sample.getNumChannels() - 1), 0, sample.getNumSamples(), (float) sampleVol.getValue());

                out.addFrom (ch, 0, bassBuffer, std::min (ch, 1), 0, bassBuffer.getNumSamples(), (float) bassVol.getValue());
            }
        }
        else
        {
            out = std::move (bassBuffer);
        }

        // Bass alone is brought to -1 dB; with the sample only lowered if it would clip.
        const auto peak = out.getMagnitude (0, out.getNumSamples());
        const auto target = withSample ? 0.98f : 0.891f;

        if (peak > 0.0f && (peak > target || ! withSample))
            out.applyGain (target / peak);

        std::vector<juce::int64> markers;
        const auto bar = 240.0 / settings.bpm * model.source.sampleRate;

        for (int b = 0; b < settings.bars; ++b)
            markers.push_back ((juce::int64) std::llround (b * bar));

        const auto name = bass::fileTag (settings) + "_" + letters[selected] + "_" + bass::toneName (bass::resolvedTone (settings))
                        + (withSample ? "_with_sample" : "");
        model.host.openAsNewFile (name, std::move (out), model.source.sampleRate, std::move (markers));
        exportStatus.setText ("Opened as a new window: " + name + " (save it with Save As)", juce::dontSendNotification);
    }

    Model& model;
    bass::Settings settings;
    std::vector<int> roots, thirds;
    std::array<std::vector<bass::Note>, 3> variants, imported;
    std::array<juce::String, 3> importedName;   // empty: the generated line
    int selected = 0;
    int currentNote = -1;   // chosen with Left / Right; -1 = none
    LayeredPlayer* previewPlayer = nullptr;   // owned by the transport while it plays

    juce::Label title, styleTitle, keyTitle, tempoTitle, downbeatTitle, downbeatValue, downbeatHelp, lengthTitle, complexityTitle,
                octaveTitle, toneTitle, harmonyTitle, chipsTitle, startInfo, sampleVolLabel, bassVolLabel, exportStatus, loopInfo;
    juce::OwnedArray<juce::TextButton> styleButtons;
    juce::ComboBox keyBox, toneBox;
    juce::TextEditor bpmField;
    juce::TextButton autoTempoButton { "Auto" }, tapButton { "Tap" };
    std::vector<double> taps;
    juce::TextButton half { juce::CharPointer_UTF8 ("\xc3\xb7" "2") }, twice { juce::CharPointer_UTF8 ("\xc3\x97" "2") };
    juce::TextButton beatBack { juce::CharPointer_UTF8 ("\xe2\x88\x92" "1 beat") }, msBack { juce::CharPointer_UTF8 ("\xe2\x88\x92" "10 ms") },
                     msZero { "0 ms" }, msForward { "+10 ms" }, beatForward { "+1 beat" };
    Segmented length { { "4 bars", "8 bars", "16 bars" } };
    Segmented complexity { { "Simple", "Medium", "Complex" } };
    Segmented octave { { "Low", "Normal", "High" } };
    Segmented harmony { { "Follow the chords", "Tonic only" } };
    BarChips chips;
    juce::TextButton fromPlayhead { "Start at the cursor" }, fromStart { "From bar 1" },
                     reroll { juce::CharPointer_UTF8 ("\xe2\x86\xbb Other variants") }, importMidi { "Import MIDI" };
    juce::OwnedArray<VariantButton> variantButtons;
    RollView roll;
    juce::TextButton previewButton;
    juce::Slider sampleVol, bassVol;
    juce::TextButton saveOne { "Save MIDI (variant A)" }, saveAll { "Save all 3 MIDI" },
                     bassToNew { "Bass to new file" }, mixToNew { "Bass + sample to new" };
    juce::Rectangle<int> settingsCard, resultCard;
};

//==============================================================================
class Tabs final : public juce::TabbedComponent
{
public:
    Tabs() : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop) {}

    std::function<void (int)> onTabChanged;

    void currentTabChanged (int index, const juce::String&) override
    {
        if (onTabChanged != nullptr)
            onTabChanged (index);
    }
};

class BassFinderPanel final : public juce::Component,
                              public SpaceTarget
{
public:
    // Space in the window: Key & Bass plays / stops the analysed audio, the
    // Note Detector does what Space does there, MIDI Bass previews the bass.
    bool spacePressed() override
    {
        switch (tabs.getCurrentTabIndex())
        {
            case 1:
                return detectorPanel->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
            case 2:
                generatorPanel->spacePlay();
                return true;
            default:
                if (model.playing != Model::Playing::nothing)
                {
                    model.stop();
                    generatorPanel->stopPreview();
                }
                else
                {
                    model.play (model.source.audio, {}, false, Model::Playing::audio, 0.0);
                }
                return true;
        }
    }

    BassFinderPanel (BassFinderSource source, BassFinderHost host, std::function<void()> onCloseIn)
        : onClose (std::move (onCloseIn))
    {
        model.source = std::move (source);
        model.host = std::move (host);
        model.analyse();

        keyPanel = std::make_unique<KeyPanel> (model);
        detectorPanel = std::make_unique<DetectorPanel> (model);
        generatorPanel = std::make_unique<GeneratorPanel> (model);

        model.keyChanged = [this]
        {
            keyPanel->repaint();
            detectorPanel->refreshKey();
        };

        tabs.addTab ("Key & Bass", colours::bg, keyPanel.get(), false);
        tabs.addTab ("Note Detector", colours::bg, detectorPanel.get(), false);
        tabs.addTab ("MIDI Bass", colours::bg, generatorPanel.get(), false);
        tabs.getTabbedButtonBar().setMinimumTabScaleFactor (1.0);
        tabs.setTabBarDepth (32);
        tabs.onTabChanged = [this] (int index)
        {
            if (index == 1)
                detectorPanel->ensureRun();
            else if (index == 2)
                generatorPanel->takeKeyboard();
        };
        addAndMakeVisible (tabs);

        styleLabel (sourceLabel, 14.0f, true);
        addAndMakeVisible (sourceLabel);
        again.onClick = [this] { analyseAgain(); };
        addAndMakeVisible (again);
        stopButton.onClick = [this] { model.stop(); generatorPanel->stopPreview(); };
        addAndMakeVisible (stopButton);
        updateSourceLabel();

        setSize (1180, 712);
    }

    ~BassFinderPanel() override
    {
        model.stop();
    }

    void paint (juce::Graphics& g) override   { g.fillAll (colours::bg); }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (40).reduced (12, 6);
        again.setBounds (top.removeFromRight (130));
        top.removeFromRight (6);
        stopButton.setBounds (top.removeFromRight (70));
        sourceLabel.setBounds (top);
        tabs.setBounds (area);
    }

    void close()
    {
        model.stop();

        if (onClose != nullptr)
            onClose();
    }

private:
    void updateSourceLabel()
    {
        sourceLabel.setText ("Analysing: " + model.source.title + "   (" + formatNumber (model.duration(), 2) + " s)",
                             juce::dontSendNotification);
    }

    void analyseAgain()
    {
        if (model.host.currentSource == nullptr)
            return;

        auto next = model.host.currentSource();

        if (! next.has_value())
            return;

        model.stop();
        generatorPanel->stopPreview();
        model.source = std::move (*next);
        model.analyse();
        updateSourceLabel();
        keyPanel->refresh();
        detectorPanel->reset();
        generatorPanel->loadFromModel();

        if (tabs.getCurrentTabIndex() == 1)
            detectorPanel->ensureRun();
    }

    Model model;
    std::function<void()> onClose;
    std::unique_ptr<KeyPanel> keyPanel;
    std::unique_ptr<DetectorPanel> detectorPanel;
    std::unique_ptr<GeneratorPanel> generatorPanel;
    Tabs tabs;
    juce::Label sourceLabel;
    juce::TextButton again { "Analyse again" }, stopButton { juce::CharPointer_UTF8 ("\xe2\x96\xa0 Stop") };
};

class BassFinderWindow final : public juce::DocumentWindow
{
public:
    BassFinderWindow (BassFinderSource source, BassFinderHost host, std::function<void()> onClose)
        : juce::DocumentWindow ("Bass Creator", colours::bg, juce::DocumentWindow::closeButton)
    {
        // Own title bar so it can be moved on window managers without decorations.
        setUsingNativeTitleBar (false);
        setLookAndFeel (&look);
        setContentOwned (new BassFinderPanel (std::move (source), std::move (host), std::move (onClose)), true);
        dark::adoptLook (*getContentComponent());
        // Fixed size so tiling window managers (Hyprland) float it instead of
        // tiling it next to the main window.
        setResizable (false, false);
        // Stays in front of the main window when you click there.
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
    }

    ~BassFinderWindow() override
    {
        // The panel uses `look`, so it goes first.
        clearContentComponent();
        setLookAndFeel (nullptr);
    }

    void closeButtonPressed() override
    {
        if (auto* panel = dynamic_cast<BassFinderPanel*> (getContentComponent()))
            panel->close();
    }

private:
    DarkLook look;
};
} // namespace

std::unique_ptr<juce::DocumentWindow> createBassFinderWindow (BassFinderSource source, BassFinderHost host,
                                                              std::function<void()> onClose)
{
    return std::make_unique<BassFinderWindow> (std::move (source), std::move (host), std::move (onClose));
}
} // namespace wf
