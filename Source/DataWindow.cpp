#include "DataWindow.h"
#include <map>
#include <set>
#include "Mp3Encoder.h"

#if ADORMIS_HAS_SAMPLERATE
 #include <samplerate.h>
#endif

namespace
{
// Whole files are edited in memory; refuse anything that would need > 2 GB.
constexpr juce::int64 maxBufferBytes = (juce::int64) 2 * 1024 * 1024 * 1024;

// Undo keeps full snapshots; drop the oldest ones beyond this budget.
constexpr size_t maxUndoBytes = (size_t) 768 * 1024 * 1024;

size_t bytesOf (const juce::AudioBuffer<float>& b)
{
    return (size_t) b.getNumChannels() * (size_t) b.getNumSamples() * sizeof (float);
}

// Integer formats: convert with the same 2^(bits-1) scale JUCE reads with, and
// round. JUCE's own float -> int path truncates with a 2^31-1 scale, which turns
// e.g. 16-bit 17318 into 17317, so an untouched file would not save back unchanged.
bool writeAudio (juce::AudioFormatWriter& writer, const juce::AudioBuffer<float>& audio)
{
    if (writer.isFloatingPoint())
        return writer.writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());

    const auto bits = writer.getBitsPerSample();
    const auto scale = (double) (1 << (bits - 1));
    const auto maxValue = (1 << (bits - 1)) - 1;
    const auto channels = audio.getNumChannels();
    constexpr int blockSize = 8192;

    juce::HeapBlock<int> data ((size_t) (channels * blockSize));
    std::vector<const int*> pointers ((size_t) channels + 1, nullptr);

    for (int c = 0; c < channels; ++c)
        pointers[(size_t) c] = data + c * blockSize;

    for (int start = 0; start < audio.getNumSamples(); start += blockSize)
    {
        const auto count = juce::jmin (blockSize, audio.getNumSamples() - start);

        for (int c = 0; c < channels; ++c)
        {
            const auto* in = audio.getReadPointer (c, start);
            auto* out = data + c * blockSize;

            for (int i = 0; i < count; ++i)   // left-justified, as AudioFormatWriter::write expects
                out[i] = (int) ((juce::uint32) juce::jlimit (-maxValue - 1, maxValue, juce::roundToInt (in[i] * scale))
                                << (32 - bits));
        }

        if (! writer.write (pointers.data(), count))
            return false;
    }

    return true;
}

// Resamples every channel of `in` from `fromRate` to `toRate`: libsamplerate's
// best sinc converter when the build has it (band-limited, so going down to a
// lower rate doesn't alias), otherwise JUCE's Lagrange interpolator.
juce::AudioBuffer<float> resampled (const juce::AudioBuffer<float>& in, double fromRate, double toRate)
{
    if (juce::approximatelyEqual (fromRate, toRate) || in.getNumSamples() == 0)
        return in;

    const auto ratio = fromRate / toRate;
    const auto outLength = (int) std::ceil (in.getNumSamples() / ratio);
    juce::AudioBuffer<float> out (in.getNumChannels(), outLength);
    out.clear();

    for (int ch = 0; ch < in.getNumChannels(); ++ch)
    {
       #if ADORMIS_HAS_SAMPLERATE
        std::unique_ptr<SRC_STATE, decltype (&src_delete)> state (nullptr, &src_delete);
        int srcError = 0;
        state.reset (src_new (SRC_SINC_BEST_QUALITY, 1, &srcError));

        if (state != nullptr)
        {
            // In blocks, so long files don't need one huge call; the last block
            // flushes the filter so the tail isn't cut off.
            constexpr long blockSize = 65536;
            long readPos = 0, written = 0;
            bool ok = true;

            while (ok && written < outLength)
            {
                SRC_DATA data {};
                data.data_in = in.getReadPointer (ch) + readPos;
                data.input_frames = juce::jmin (blockSize, (long) in.getNumSamples() - readPos);
                data.data_out = out.getWritePointer (ch) + written;
                data.output_frames = outLength - written;
                data.end_of_input = readPos + data.input_frames >= in.getNumSamples() ? 1 : 0;
                data.src_ratio = toRate / fromRate;

                ok = src_process (state.get(), &data) == 0;
                readPos += data.input_frames_used;
                written += data.output_frames_gen;

                if (data.end_of_input != 0 && data.output_frames_gen == 0)
                    break;
            }

            if (ok)
                continue;

            out.clear (ch, 0, outLength);
        }
       #endif

        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio, in.getReadPointer (ch), out.getWritePointer (ch), outLength,
                              in.getNumSamples(), 0);
    }

    return out;
}

template <typename Snapshot>
size_t bytesOf (const Snapshot& s)
{
    return bytesOf (s.audio);
}
} // namespace

std::unique_ptr<DataWindow> DataWindow::load (const juce::File& file, juce::AudioFormatManager& formats, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        error = "Can't open \"" + file.getFileName() + "\": unsupported or damaged file.";
        return nullptr;
    }

    const auto length = reader->lengthInSamples;
    const auto channels = (int) reader->numChannels;

    if (length <= 0 || channels <= 0 || length > std::numeric_limits<int>::max()
        || length * channels * (juce::int64) sizeof (float) > maxBufferBytes)
    {
        error = "\"" + file.getFileName() + "\" is empty or too large to edit.";
        return nullptr;
    }

    std::unique_ptr<DataWindow> doc (new DataWindow());
    doc->file = file;
    doc->sampleRate = reader->sampleRate;
    doc->bitDepth = (int) reader->bitsPerSample;
    doc->floatingPoint = reader->usesFloatingPointData;
    doc->formatName = reader->getFormatName();
    doc->buffer.setSize (channels, (int) length);
    reader->read (&doc->buffer, 0, (int) length, 0, true, true);
    doc->readCues (reader->metadataValues);

    doc->initialise (file.getFileName());
    return doc;
}

std::unique_ptr<DataWindow> DataWindow::fromSelection (const DataWindow& source, const juce::String& newTitle)
{
    const auto selection = source.view.getSelection();

    if (selection.isEmpty())
        return nullptr;

    std::unique_ptr<DataWindow> doc (new DataWindow());
    doc->sampleRate = source.sampleRate;
    doc->bitDepth = source.bitDepth;
    doc->floatingPoint = source.floatingPoint;
    doc->formatName = source.formatName;

    // One picked channel becomes a mono file.
    doc->buffer = source.copySelection();

    // The markers and regions inside it come along.
    const auto marks = source.selectionMarks();
    doc->markers = marks.markers;
    doc->barStarts = marks.barStarts;
    doc->regions = marks.regions;

    doc->modified = true;
    doc->initialise (newTitle);
    return doc;
}

void DataWindow::initialise (const juce::String& newTitle)
{
    title = newTitle;
    setName (modified ? "*" + title : title);
    view.setAudio (&buffer, sampleRate);
    view.onCursorOrSelectionChanged = [this] { updateInfo(); if (onChanged) onChanged(); };
    view.onPlayheadMoved = [this] { updateInfo(); };
    view.onRemoveMarker = [this] (int index) { removeMarker (index); };
    view.onMoveMarker = [this] (int index, juce::int64 position) { moveMarker (index, position); };
    view.setMarkers (&markers, &barStarts);
    view.setRegions (&regions);
    view.onRenameRegion = [this] (int index) { if (onRegionAction) onRegionAction (index, false); };
    view.onRegionMenu = [this] (int index) { if (onRegionAction) onRegionAction (index, true); };
    addAndMakeVisible (view);

    for (auto* label : { &resolutionLabel, &lengthBox, &cursorBox })
    {
        label->setFont (juce::FontOptions (14.0f));
        label->setColour (juce::Label::textColourId, juce::Colours::black);
        label->setMinimumHorizontalScale (1.0f);
        label->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
    }

    for (auto* box : { &lengthBox, &cursorBox })
    {
        box->setJustificationType (juce::Justification::centredRight);
        box->setColour (juce::Label::backgroundColourId, juce::Colours::white);
        box->setColour (juce::Label::outlineColourId, juce::Colour (0xff808080));
    }

    lengthBox.setTooltip ("Length of the file");
    cursorBox.setTooltip ("Cursor position");
    updateInfo();
}

bool DataWindow::isSaveExtension (const juce::String& extension)
{
    if (extension.equalsIgnoreCase (".mp3"))
        return wf::mp3EncodingAvailable();

    return juce::StringArray { ".wav", ".flac", ".aiff", ".aif", ".ogg" }.contains (extension, true);
}

bool DataWindow::canSaveInPlace() const
{
    const auto ext = file.getFileExtension();
    return file.existsAsFile()
        && (ext.equalsIgnoreCase (".wav") || ext.equalsIgnoreCase (".flac")
            || ext.equalsIgnoreCase (".aiff") || ext.equalsIgnoreCase (".aif"));
}

bool DataWindow::saveTo (const juce::File& destination, juce::AudioFormatManager& formats, juce::String& error,
                         const wf::SaveFormat* chosen)
{
    // Another sample rate: convert first, and the document takes the converted
    // audio over only once the file is written.
    const auto targetRate = chosen != nullptr && chosen->sampleRate > 0.0 ? chosen->sampleRate : sampleRate;
    const auto convert = ! juce::approximatelyEqual (targetRate, sampleRate);
    auto converted = convert ? resampled (buffer, sampleRate, targetRate) : juce::AudioBuffer<float>();
    const auto& audio = convert ? converted : buffer;

    auto markSaved = [this, &destination, convert, targetRate, &converted] (const juce::String& newFormatName,
                                                                           int newBitDepth, bool newFloatingPoint)
    {
        if (convert)
        {
            pushUndo();

            const auto ratio = targetRate / sampleRate;
            remapMarkers ([ratio] (juce::int64 m) { return std::optional<juce::int64> (std::llround ((double) m * ratio)); });

            buffer = std::move (converted);
            sampleRate = targetRate;
            view.setAudio (&buffer, sampleRate);
        }

        file = destination;
        title = destination.getFileName();
        formatName = newFormatName;
        // Saved at another depth: the 12 / 14-bit label no longer applies.
        if (newBitDepth != bitDepth)
            resolutionBits = 0;

        bitDepth = newBitDepth;
        floatingPoint = newFloatingPoint;
        modified = false;
        setName (title);
        updateInfo();

        if (onChanged)
            onChanged();
    };

    // MP3 goes through LAME; JUCE can only read MP3.
    if (destination.hasFileExtension (".mp3"))
    {
        if (! wf::writeMp3 (destination, audio, targetRate, chosen != nullptr && chosen->kbps > 0 ? chosen->kbps : 320, error))
            return false;

        markSaved ("MP3 file", 0, false);
        return true;
    }

    auto* format = isSaveExtension (destination.getFileExtension())
                       ? formats.findFormatForFileExtension (destination.getFileExtension()) : nullptr;

    if (format == nullptr)
    {
        error = "Can't save as \"" + destination.getFileExtension() + "\". Choose WAV, FLAC, AIFF, MP3 or OGG.";
        return false;
    }

    // The chosen depth, or keep the original one when the format allows it, otherwise 24-bit.
    const auto depths = format->getPossibleBitDepths();
    const auto wanted = chosen != nullptr ? chosen->bitsPerSample : bitDepth;
    auto bits = depths.contains (wanted) ? wanted : depths.contains (24) ? 24 : depths.getLast();

    auto options = juce::AudioFormatWriterOptions{}.withSampleRate (targetRate)
                                                   .withNumChannels (audio.getNumChannels())
                                                   .withBitsPerSample (bits);

    // WAV keeps the markers and regions (cue points and labels, as Sound
    // Forge and most editors read them).
    if (destination.hasFileExtension (".wav") && (! markers.empty() || ! regions.empty()))
        options = options.withMetadataValues (cueMetadata (convert ? targetRate / sampleRate : 1.0));

    // Save (no choice made) keeps float data float, as long as it stays 32-bit.
    const auto useFloat = bits == 32 && (chosen != nullptr ? chosen->floatingPoint : floatingPoint);

    if (useFloat)
        options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    // Lossy formats: the chosen bitrate, or about 192 kbps.
    const auto qualities = format->getQualityOptions();
    if (! qualities.isEmpty())
    {
        const auto wantedKbps = juce::String (chosen != nullptr && chosen->kbps > 0 ? chosen->kbps : 192) + " kbps";
        const auto preferred = qualities.indexOf (wantedKbps);
        options = options.withQualityOptionIndex (preferred >= 0 ? preferred : qualities.size() / 2);
    }

    // Write to a temporary file first so a failed save never damages the original.
    juce::TemporaryFile temp (destination);
    std::unique_ptr<juce::OutputStream> stream = temp.getFile().createOutputStream();

    if (stream == nullptr)
    {
        error = "Can't write to \"" + destination.getFullPathName() + "\".";
        return false;
    }

    auto writer = format->createWriterFor (stream, options);

    if (writer == nullptr || ! writeAudio (*writer, audio))
    {
        error = "Saving \"" + destination.getFileName() + "\" failed.";
        return false;
    }

    writer.reset();   // flushes and closes the stream

    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "Can't replace \"" + destination.getFullPathName() + "\".";
        return false;
    }

    markSaved (format->getFormatName(), bits, useFloat);
    return true;
}

juce::Range<int> DataWindow::processRange() const
{
    const auto selection = view.getSelection();

    if (selection.isEmpty())
        return { 0, buffer.getNumSamples() };

    return { (int) selection.getStart(), (int) selection.getEnd() };
}

void DataWindow::processChannels (const std::function<void (juce::AudioBuffer<float>&, juce::Range<int>)>& fn)
{
    pushUndo();

    // A selection on one channel processes only that channel.
    if (const auto picked = view.getSelectedChannel(); picked >= 0 && picked < buffer.getNumChannels())
    {
        float* channel[] = { buffer.getWritePointer (picked) };
        juce::AudioBuffer<float> one (channel, 1, buffer.getNumSamples());
        fn (one, processRange());
    }
    else
    {
        fn (buffer, processRange());
    }

    audioChanged();
}

void DataWindow::applyProcess (wf::Process process)
{
    processChannels ([process] (auto& audio, auto range) { wf::applyProcess (process, audio, range); });
}

void DataWindow::applyVolume (float decibels)
{
    processChannels ([decibels] (auto& audio, auto range) { wf::applyVolume (audio, range, decibels); });
}

void DataWindow::replaceRange (juce::Range<int> range, const juce::AudioBuffer<float>& audio, double timeRatio)
{
    range = range.getIntersectionWith ({ 0, buffer.getNumSamples() });

    if (audio.getNumChannels() != buffer.getNumChannels() || range.isEmpty())
        return;

    const auto wasSelection = ! view.getSelection().isEmpty();
    const auto newLength = audio.getNumSamples();
    const auto tail = buffer.getNumSamples() - range.getEnd();

    pushUndo();

    juce::AudioBuffer<float> result (buffer.getNumChannels(), range.getStart() + newLength + tail);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        result.copyFrom (ch, 0, buffer, ch, 0, range.getStart());
        result.copyFrom (ch, range.getStart(), audio, ch, 0, newLength);
        result.copyFrom (ch, range.getStart() + newLength, buffer, ch, range.getEnd(), tail);
    }

    buffer = std::move (result);

    const auto shift = (juce::int64) newLength - range.getLength();

    remapMarkers ([range, shift, newLength, timeRatio] (juce::int64 m) -> std::optional<juce::int64>
    {
        if (m >= range.getEnd() && m > range.getStart())
            return m + shift;

        if (m > range.getStart())
            return range.getStart() + std::min ((juce::int64) newLength,
                                                (juce::int64) std::llround ((double) (m - range.getStart()) * timeRatio));
        return m;
    });

    view.setSelection (wasSelection ? juce::Range<juce::int64> (range.getStart(), range.getStart() + newLength)
                                    : juce::Range<juce::int64>());
    view.setCursor (range.getStart());
    audioChanged();
}

void DataWindow::applyEffect (juce::Range<int> range, const juce::AudioBuffer<float>& processed)
{
    range = range.getIntersectionWith ({ 0, buffer.getNumSamples() });

    if (range.isEmpty() || processed.getNumChannels() != buffer.getNumChannels() || processed.getNumSamples() < range.getLength())
        return;

    pushUndo();
    const auto tail = processed.getNumSamples() - range.getLength();

    if (range.getEnd() + tail > buffer.getNumSamples())
        buffer.setSize (buffer.getNumChannels(), range.getEnd() + tail, true, true, false);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        buffer.copyFrom (ch, range.getStart(), processed, ch, 0, range.getLength());

        if (tail > 0)
            buffer.addFrom (ch, range.getEnd(), processed, ch, range.getLength(), tail);
    }

    audioChanged();
}

std::optional<wf::AudioStatistics> DataWindow::measureTarget (const std::function<bool (float)>& progress) const
{
    if (const auto picked = view.getSelectedChannel(); picked >= 0 && picked < buffer.getNumChannels())
    {
        float* channel[] = { const_cast<float*> (buffer.getReadPointer (picked)) };
        const juce::AudioBuffer<float> one (channel, 1, buffer.getNumSamples());
        return wf::measure (one, sampleRate, processRange(), progress);
    }

    return wf::measure (buffer, sampleRate, processRange(), progress);
}

DataWindow::NormalizeResult DataWindow::normalize (bool loudness, double target, double truePeakCeiling)
{
    NormalizeResult result;
    const auto stats = measureTarget();

    if (! stats.has_value())
        return result;

    double peak = wf::LoudnessMeter::silence;

    for (const auto& c : stats->channels)
        peak = juce::jmax (peak, c.peakDb);

    if (loudness)
    {
        if (stats->integrated <= -70.0)
        {
            result.silent = true;
            return result;
        }

        result.gainDb = target - stats->integrated;

        if (stats->truePeakDb + result.gainDb > truePeakCeiling)
        {
            result.gainDb = truePeakCeiling - stats->truePeakDb;
            result.limited = true;
        }

        result.reached = stats->integrated + result.gainDb;
    }
    else
    {
        if (peak <= -100.0)
        {
            result.silent = true;
            return result;
        }

        result.gainDb = target - peak;
        result.reached = target;
    }

    result.truePeak = stats->truePeakDb + result.gainDb;

    if (std::abs (result.gainDb) > 0.0005)
        applyVolume ((float) result.gainDb);

    return result;
}

juce::String DataWindow::describeProcessTarget() const
{
    const auto channel = view.getSelectedChannel();
    const auto channelText = channel < 0 ? juce::String()
                           : channel == 0 ? juce::String (", left channel") : juce::String (", right channel");

    return (view.getSelection().isEmpty() ? juce::String ("whole file") : juce::String ("selection")) + channelText;
}

bool DataWindow::deleteSelection()
{
    const auto selection = view.getSelection();

    // Cutting one channel would leave the channels different lengths.
    if (selection.isEmpty() || view.getSelectedChannel() >= 0)
        return false;

    const auto start = (int) selection.getStart();
    const auto removed = (int) selection.getLength();
    const auto remaining = buffer.getNumSamples() - removed;

    pushUndo();

    juce::AudioBuffer<float> result (buffer.getNumChannels(), remaining);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        result.copyFrom (ch, 0, buffer, ch, 0, start);
        result.copyFrom (ch, start, buffer, ch, start + removed, remaining - start);
    }

    buffer = std::move (result);

    // Markers inside the cut go away; later ones move back with the audio.
    remapMarkers ([start, removed] (juce::int64 m) -> std::optional<juce::int64>
    {
        if (m < start)              return m;
        if (m >= start + removed)   return m - removed;
        return {};
    });
    view.setSelection ({});
    view.setCursor (start);
    audioChanged();
    return true;
}

std::unique_ptr<DataWindow> DataWindow::createSilent (const juce::String& newTitle, double rate, int channels,
                                                      juce::int64 numSamples, std::vector<juce::int64> newMarkers,
                                                      std::vector<juce::int64> newBarStarts, int bits)
{
    if (numSamples <= 0 || numSamples > std::numeric_limits<int>::max() || channels < 1)
        return nullptr;

    std::unique_ptr<DataWindow> doc (new DataWindow());
    doc->sampleRate = rate;
    doc->bitDepth = 16;
    doc->formatName = "WAV file";
    doc->buffer.setSize (channels, (int) numSamples);
    doc->buffer.clear();
    doc->setResolution (bits);
    doc->markers = std::move (newMarkers);
    doc->barStarts = std::move (newBarStarts);
    std::sort (doc->barStarts.begin(), doc->barStarts.end());
    doc->modified = true;
    doc->initialise (newTitle);
    return doc;
}

std::unique_ptr<DataWindow> DataWindow::createEmpty (const juce::String& newTitle, double rate, int channels)
{
    std::unique_ptr<DataWindow> doc (new DataWindow());
    doc->sampleRate = rate;
    doc->bitDepth = 24;
    doc->formatName = "WAV file";
    doc->buffer.setSize (juce::jlimit (1, 2, channels), 0);
    doc->initialise (newTitle);
    doc->view.setEmptyText ("Empty - press Record (Ctrl+R)");
    return doc;
}

void DataWindow::setEmptyFormat (double rate, int channels)
{
    if (! isEmpty())
        return;

    buffer.setSize (juce::jlimit (1, 2, channels), 0);
    sampleRate = rate;
    view.setAudio (&buffer, sampleRate);
    updateInfo();
}

const std::vector<int>& DataWindow::resolutionChoices()
{
    static const std::vector<int> choices { 8, 12, 14, 16, 24, 32 };
    return choices;
}

void DataWindow::setResolution (int bits)
{
    resolutionBits = bits == 12 || bits == 14 ? bits : 0;
    bitDepth = bits == 12 || bits == 14 ? 16 : bits;
    floatingPoint = bits == 32;
    updateInfo();
}

void DataWindow::quantize (juce::AudioBuffer<float>& audio, int start, int numSamples, int bits)
{
    if (bits >= 32 || bits < 2 || numSamples <= 0)
        return;

    const auto steps = (float) (1 << (bits - 1));
    const auto top = (steps - 1.0f) / steps;

    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        auto* d = audio.getWritePointer (ch, start);

        for (int i = 0; i < numSamples; ++i)
            d[i] = juce::jlimit (-1.0f, top, std::round (d[i] * steps) / steps);
    }
}

void DataWindow::beginTake()
{
    pushUndo();
    takeStart = buffer.getNumSamples();
}

void DataWindow::appendAudio (const juce::AudioBuffer<float>& audio, int numSamples)
{
    if (numSamples <= 0)
        return;

    const auto start = buffer.getNumSamples();
    const auto needed = start + numSamples;

    // Grow the memory in big steps (doubling, at least 10 s, at most a minute
    // at a time) so a long recording isn't copied over and over.
    if ((size_t) needed * sizeof (float) * (size_t) buffer.getNumChannels() > capacityBytes)
    {
        const auto step = std::clamp (start, (int) (sampleRate * 10.0), (int) (sampleRate * 60.0));
        const auto capacity = std::max (needed, start + step);
        buffer.setSize (buffer.getNumChannels(), capacity, true, true, false);
        capacityBytes = (size_t) capacity * sizeof (float) * (size_t) buffer.getNumChannels();
    }

    buffer.setSize (buffer.getNumChannels(), needed, true, false, true);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom (ch, start, audio, std::min (ch, audio.getNumChannels() - 1), 0, numSamples);

    quantize (buffer, start, numSamples, getResolution());
    modified = true;
    setName ("*" + title);
    updateInfo();
    view.audioChanged();
    view.zoomToFit();
}

void DataWindow::endTake()
{
    // A take that recorded nothing leaves no undo step behind; otherwise a
    // marker shows where it stopped (and where the next take will continue).
    // Undoing the take removes the marker with it.
    if (buffer.getNumSamples() == takeStart && ! undoStack.empty())
        undoStack.pop_back();
    else
        addMarker (buffer.getNumSamples());

    // Give back the memory reserved beyond the end.
    // (A copy is allocated at the exact size; setSize to the same size keeps it all.)
    if (capacityBytes > 0)
    {
        juce::AudioBuffer<float> fitted (buffer);
        buffer = std::move (fitted);
    }

    capacityBytes = 0;
    audioChanged();
}

std::unique_ptr<DataWindow> DataWindow::fromAudio (const juce::String& newTitle, double rate, juce::AudioBuffer<float> audio,
                                                   std::vector<juce::int64> newMarkers, std::vector<juce::int64> newBarStarts)
{
    if (audio.getNumSamples() <= 0 || audio.getNumChannels() < 1)
        return nullptr;

    std::unique_ptr<DataWindow> doc (new DataWindow());
    doc->sampleRate = rate;
    doc->bitDepth = 24;
    doc->formatName = "WAV file";
    doc->buffer = std::move (audio);
    doc->markers = std::move (newMarkers);
    doc->barStarts = std::move (newBarStarts);
    std::sort (doc->barStarts.begin(), doc->barStarts.end());
    doc->modified = true;
    doc->initialise (newTitle);
    return doc;
}

juce::AudioBuffer<float> DataWindow::copySelection() const
{
    const auto selection = view.getSelection();
    const auto picked = view.getSelectedChannel();
    const auto first = picked >= 0 ? picked : 0;
    const auto channels = picked >= 0 ? 1 : buffer.getNumChannels();
    juce::AudioBuffer<float> copy (channels, (int) selection.getLength());

    for (int ch = 0; ch < channels; ++ch)
        copy.copyFrom (ch, 0, buffer, first + ch, (int) selection.getStart(), (int) selection.getLength());

    return copy;
}

namespace
{
// Source sample for destination channel `ch`: mono fans out to every channel,
// and a source with more channels than the destination is averaged down.
float sourceSample (const juce::AudioBuffer<float>& src, int ch, int destChannels, int index)
{
    const auto srcChannels = src.getNumChannels();

    if (srcChannels == 1)
        return src.getSample (0, index);

    if (destChannels == 1)
    {
        float sum = 0.0f;
        for (int c = 0; c < srcChannels; ++c)
            sum += src.getSample (c, index);
        return sum / (float) srcChannels;
    }

    return ch < srcChannels ? src.getSample (ch, index) : 0.0f;
}
} // namespace

juce::AudioBuffer<float> DataWindow::prepareForMix (const juce::AudioBuffer<float>& source, double sourceRate,
                                                    double rate, int channels)
{
    const auto src = resampled (source, sourceRate, rate);
    juce::AudioBuffer<float> out (channels, src.getNumSamples());

    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < src.getNumSamples(); ++i)
            out.setSample (ch, i, sourceSample (src, ch, channels, i));

    return out;
}

DataWindow::Marks DataWindow::selectionMarks() const
{
    Marks m;
    m.sampleRate = sampleRate;
    const auto selection = view.getSelection();

    if (selection.isEmpty())
        return m;

    const auto s = selection.getStart(), e = selection.getEnd();

    // Its edges count (a selection between two markers keeps both).
    for (auto x : markers)
        if (x >= s && x <= e)
            m.markers.push_back (x - s);

    for (auto x : barStarts)
        if (x >= s && x <= e)
            m.barStarts.push_back (x - s);

    for (const auto& r : regions)
    {
        const auto a = std::max (r.start, s), b = std::min (r.end, e);

        if (b > a)
            m.regions.push_back ({ a - s, b - s, r.name });
    }

    return m;
}

void DataWindow::addMarks (const Marks& marks, juce::int64 offset)
{
    const auto ratio = sampleRate / marks.sampleRate;
    const auto total = (juce::int64) buffer.getNumSamples();
    auto at = [&] (juce::int64 x) { return juce::jlimit ((juce::int64) 0, total, offset + (juce::int64) std::llround ((double) x * ratio)); };

    for (auto x : marks.markers)
        if (const auto p = at (x); ! std::binary_search (markers.begin(), markers.end(), p))
            markers.insert (std::lower_bound (markers.begin(), markers.end(), p), p);

    for (auto x : marks.barStarts)
        if (const auto p = at (x); ! std::binary_search (barStarts.begin(), barStarts.end(), p))
            barStarts.insert (std::lower_bound (barStarts.begin(), barStarts.end(), p), p);

    for (const auto& r : marks.regions)
        if (const auto a = at (r.start), b = at (r.end); b > a)
            regions.push_back ({ a, b, r.name });

    std::stable_sort (regions.begin(), regions.end(), [] (const auto& x, const auto& y) { return x.start < y.start; });
    view.setRegions (&regions);
    view.repaint();
}

void DataWindow::mixIn (const juce::AudioBuffer<float>& source, double sourceRate, juce::int64 position,
                        float sourceGain, float destGain, const Marks* marks)
{
    // An empty file takes the format of what comes in.
    setEmptyFormat (sourceRate, source.getNumChannels());
    const auto channels = buffer.getNumChannels();
    const auto src = prepareForMix (source, sourceRate, sampleRate, channels);
    const auto start = (int) juce::jlimit ((juce::int64) 0, (juce::int64) buffer.getNumSamples(), position);
    const auto length = src.getNumSamples();

    if (length == 0)
        return;

    pushUndo();

    if (start + length > buffer.getNumSamples())
        buffer.setSize (channels, start + length, true, true);

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* dest = buffer.getWritePointer (ch, start);
        const auto* in = src.getReadPointer (ch);

        for (int i = 0; i < length; ++i)
            dest[i] = dest[i] * destGain + in[i] * sourceGain;
    }

    if (marks != nullptr)
        addMarks (*marks, start);

    view.setSelection ({ start, start + length });
    audioChanged();
}

void DataWindow::pasteAudio (const juce::AudioBuffer<float>& source, double sourceRate, const Marks* marks)
{
    // An empty file takes the format of what comes in.
    setEmptyFormat (sourceRate, source.getNumChannels());
    const auto channels = buffer.getNumChannels();
    const auto src = prepareForMix (source, sourceRate, sampleRate, channels);
    const auto length = src.getNumSamples();

    if (length == 0)
        return;

    const auto selection = view.getSelection();
    const auto start = (int) juce::jlimit ((juce::int64) 0, (juce::int64) buffer.getNumSamples(),
                                           selection.isEmpty() ? view.getCursor() : selection.getStart());
    const auto removed = selection.isEmpty() ? 0 : (int) selection.getLength();
    const auto after = buffer.getNumSamples() - start - removed;

    pushUndo();

    juce::AudioBuffer<float> result (channels, start + length + after);

    for (int ch = 0; ch < channels; ++ch)
    {
        result.copyFrom (ch, 0, buffer, ch, 0, start);
        result.copyFrom (ch, start, src, ch, 0, length);
        result.copyFrom (ch, start + length, buffer, ch, start + removed, after);
    }

    buffer = std::move (result);

    // Markers in a replaced selection go away; later ones move with the audio.
    remapMarkers ([start, removed, length] (juce::int64 m) -> std::optional<juce::int64>
    {
        if (m < start || (removed == 0 && m == start))   return m;
        if (m >= start + removed)                        return m - removed + length;
        return {};
    });

    if (marks != nullptr)
        addMarks (*marks, start);

    view.setSelection ({ start, start + length });
    audioChanged();
}

bool DataWindow::trimToSelection()
{
    const auto selection = view.getSelection();

    if (selection.isEmpty() || view.getSelectedChannel() >= 0)
        return false;

    const auto start = (int) selection.getStart();
    const auto length = (int) selection.getLength();

    pushUndo();

    juce::AudioBuffer<float> result (buffer.getNumChannels(), length);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        result.copyFrom (ch, 0, buffer, ch, start, length);

    buffer = std::move (result);

    remapMarkers ([start, length] (juce::int64 m) -> std::optional<juce::int64>
    {
        if (m < start || m > start + length)
            return {};

        return m - start;
    });
    view.setSelection ({ 0, length });
    audioChanged();
    return true;
}

void DataWindow::insertSilence (juce::int64 position, double seconds)
{
    const auto length = (int) std::llround (seconds * sampleRate);

    if (length <= 0)
        return;

    juce::AudioBuffer<float> silence (buffer.getNumChannels(), length);
    silence.clear();
    view.setSelection ({});
    view.setCursor (juce::jlimit ((juce::int64) 0, (juce::int64) buffer.getNumSamples(), position));
    pasteAudio (silence, sampleRate);
}

void DataWindow::resample (double newRate)
{
    if (newRate <= 0.0 || juce::approximatelyEqual (newRate, sampleRate))
        return;

    pushUndo();
    const auto ratio = newRate / sampleRate;
    buffer = resampled (buffer, sampleRate, newRate);
    remapMarkers ([ratio] (juce::int64 m) { return std::optional<juce::int64> (std::llround ((double) m * ratio)); });
    sampleRate = newRate;
    view.setAudio (&buffer, sampleRate);
    audioChanged();
}

void DataWindow::convertBitDepth (int bits, bool dither)
{
    pushUndo();

    if (bits < 32)
    {
        const auto steps = (float) (1 << (bits - 1));

        if (dither)
        {
            // TPDF dither: two uniform randoms, one step peak to peak each.
            juce::Random random (0x5eed);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* d = buffer.getWritePointer (ch);

                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    d[i] += (random.nextFloat() - random.nextFloat()) / steps;
            }
        }

        quantize (buffer, 0, buffer.getNumSamples(), bits);
    }

    setResolution (bits);
    audioChanged();
}

bool DataWindow::convertChannels (ChannelConversion conversion)
{
    const auto channels = buffer.getNumChannels();
    const auto n = buffer.getNumSamples();

    if ((conversion == ChannelConversion::monoToStereo) != (channels == 1) || channels > 2)
        return false;

    juce::AudioBuffer<float> result;

    switch (conversion)
    {
        case ChannelConversion::stereoToMono:
            result.setSize (1, n);
            result.copyFrom (0, 0, buffer, 0, 0, n);
            result.addFrom (0, 0, buffer, 1, 0, n);
            result.applyGain (0.5f);
            break;

        case ChannelConversion::monoToStereo:
            result.setSize (2, n);
            result.copyFrom (0, 0, buffer, 0, 0, n);
            result.copyFrom (1, 0, buffer, 0, 0, n);
            break;

        case ChannelConversion::swap:
            result.setSize (2, n);
            result.copyFrom (0, 0, buffer, 1, 0, n);
            result.copyFrom (1, 0, buffer, 0, 0, n);
            break;

        case ChannelConversion::leftOnly:
        case ChannelConversion::rightOnly:
            result.setSize (1, n);
            result.copyFrom (0, 0, buffer, conversion == ChannelConversion::leftOnly ? 0 : 1, 0, n);
            break;
    }

    pushUndo();
    buffer = std::move (result);
    view.setSelection ({});
    view.setAudio (&buffer, sampleRate);
    audioChanged();
    return true;
}

bool DataWindow::autoTrim (float thresholdDb)
{
    const auto level = juce::Decibels::decibelsToGain (thresholdDb);
    const auto n = buffer.getNumSamples();

    auto loud = [&] (int i)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            if (std::abs (buffer.getSample (ch, i)) > level)
                return true;

        return false;
    };

    int first = 0, last = n - 1;
    while (first < n && ! loud (first))   ++first;
    while (last > first && ! loud (last)) --last;

    if (first >= n || (first == 0 && last == n - 1))
        return false;

    view.setSelection ({ first, last + 1 });
    return trimToSelection();
}

int DataWindow::markClipping (float levelDb, int minRun)
{
    const auto level = juce::Decibels::decibelsToGain (levelDb) - 1.0e-6f;
    const auto n = buffer.getNumSamples();
    const auto mergeGap = (int) (sampleRate * 0.05);   // clips closer than 50 ms count as one
    int found = 0;
    juce::int64 lastMarked = -mergeGap - 1;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* d = buffer.getReadPointer (ch);
        int run = 0;

        for (int i = 0; i < n; ++i)
        {
            run = std::abs (d[i]) >= level ? run + 1 : 0;

            if (run == minRun)
            {
                const auto start = (juce::int64) (i - minRun + 1);
                const auto near = std::any_of (markers.begin(), markers.end(),
                                               [&] (juce::int64 m) { return std::abs (m - start) <= mergeGap; });

                if (! near && std::abs (start - lastMarked) > mergeGap)
                {
                    addMarker (start);
                    ++found;
                    lastMarked = start;
                }
            }
        }
    }

    return found;
}

bool DataWindow::copyOtherChannel()
{
    const auto channel = view.getSelectedChannel();

    if (buffer.getNumChannels() != 2 || channel < 0)
        return false;

    const auto range = processRange();
    pushUndo();
    buffer.copyFrom (channel, range.getStart(), buffer, 1 - channel, range.getStart(), range.getLength());
    audioChanged();
    return true;
}

bool DataWindow::deleteSelectedMarkers()
{
    const auto& picked = view.getSelectedMarkers();
    std::vector<juce::int64> kept;

    for (auto m : markers)
        if (std::find (picked.begin(), picked.end(), m) == picked.end())
            kept.push_back (m);

    if (kept.size() == markers.size())
        return false;

    // Markers aren't saved in the audio file, so this doesn't mark it modified.
    pushUndo();
    markers = std::move (kept);
    pruneBarStarts();
    view.clearMarkerSelection();
    view.repaint();

    if (onChanged)
        onChanged();

    return true;
}

void DataWindow::addMarker (juce::int64 position)
{
    position = juce::jlimit ((juce::int64) 0, (juce::int64) buffer.getNumSamples(), position);
    const auto it = std::lower_bound (markers.begin(), markers.end(), position);

    if (it != markers.end() && *it == position)
        return;

    markers.insert (it, position);
    view.clearMarkerSelection();
    view.repaint();
}

void DataWindow::moveMarker (int index, juce::int64 position)
{
    if (! juce::isPositiveAndBelow (index, (int) markers.size()))
        return;

    position = juce::jlimit ((juce::int64) 0, (juce::int64) buffer.getNumSamples(), position);
    const auto from = markers[(size_t) index];

    if (position == from)
        return;

    pushUndo();
    markers.erase (markers.begin() + index);

    // A bar-start marker stays one where it lands.
    if (const auto bar = std::lower_bound (barStarts.begin(), barStarts.end(), from); bar != barStarts.end() && *bar == from)
    {
        barStarts.erase (bar);
        barStarts.insert (std::lower_bound (barStarts.begin(), barStarts.end(), position), position);
    }

    // Dropped onto another marker: the two become one.
    if (! std::binary_search (markers.begin(), markers.end(), position))
        markers.insert (std::lower_bound (markers.begin(), markers.end(), position), position);

    pruneBarStarts();
    regionsChanged();   // marks the file changed (markers are saved in WAV)
}

void DataWindow::removeMarker (int index)
{
    if (juce::isPositiveAndBelow (index, (int) markers.size()))
    {
        markers.erase (markers.begin() + index);
        pruneBarStarts();
        view.clearMarkerSelection();
        view.repaint();
    }
}

void DataWindow::remapMarkers (const std::function<std::optional<juce::int64> (juce::int64)>& fn)
{
    auto remap = [&fn] (std::vector<juce::int64>& list)
    {
        std::vector<juce::int64> moved;

        for (auto m : list)
            if (const auto to = fn (m))
                moved.push_back (*to);

        std::sort (moved.begin(), moved.end());
        list = std::move (moved);
    };

    remap (markers);
    remap (barStarts);

    // A region's edge in removed audio moves to the nearest audio kept inside
    // it (the removed part is one stretch, so a binary search finds it).
    std::vector<AudioRegion> moved;

    for (auto r : regions)
    {
        auto start = fn (r.start), end = fn (r.end);

        if (! start.has_value())
        {
            juce::int64 lo = r.start, hi = r.end;   // first p in (start, end] that maps

            if (fn (hi).has_value())
            {
                while (hi - lo > 1)
                {
                    const auto mid = lo + (hi - lo) / 2;
                    (fn (mid).has_value() ? hi : lo) = mid;
                }

                start = fn (hi);
            }
        }

        if (! end.has_value())
        {
            juce::int64 lo = r.start, hi = r.end;   // last p in [start, end) that maps

            if (fn (lo).has_value())
            {
                while (hi - lo > 1)
                {
                    const auto mid = lo + (hi - lo) / 2;
                    (fn (mid).has_value() ? lo : hi) = mid;
                }

                // Just past the last sample kept (the end is exclusive).
                if (const auto last = fn (lo))
                    end = std::min (*last + 1, (juce::int64) buffer.getNumSamples());
            }
        }

        if (start.has_value() && end.has_value() && *end > *start)
            moved.push_back ({ *start, *end, r.name });
    }

    std::sort (moved.begin(), moved.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
    regions = std::move (moved);
    view.repaint();
}

namespace
{
constexpr juce::uint32 regionPurpose = 0x206e6772;   // "rgn " (little-endian), what editors use for regions
} // namespace

std::unordered_map<juce::String, juce::String> DataWindow::cueMetadata (double ratio) const
{
    std::unordered_map<juce::String, juce::String> v;
    auto at = [ratio] (juce::int64 p) { return juce::String ((juce::int64) std::llround ((double) p * ratio)); };
    int cue = 0, label = 0;

    auto addCue = [&] (int id, juce::int64 position, const juce::String& text)
    {
        const auto c = "Cue" + juce::String (cue++);
        v[c + "Identifier"] = juce::String (id);
        v[c + "Offset"] = at (position);
        v[c + "SampleOffset"] = at (position);
        const auto l = "CueLabel" + juce::String (label++);
        v[l + "Identifier"] = juce::String (id);
        v[l + "Text"] = text;
    };

    for (int i = 0; i < (int) markers.size(); ++i)
        addCue (i + 1, markers[(size_t) i], "Marker " + juce::String (i + 1));

    for (int i = 0; i < (int) regions.size(); ++i)
    {
        const auto& r = regions[(size_t) i];
        const auto id = (int) markers.size() + i + 1;
        addCue (id, r.start, r.name);
        const auto k = "CueRegion" + juce::String (i);
        v[k + "Identifier"] = juce::String (id);
        v[k + "SampleLength"] = juce::String ((juce::int64) std::llround ((double) (r.end - r.start) * ratio));
        v[k + "Purpose"] = juce::String ((juce::int64) regionPurpose);
        v[k + "Text"] = r.name;
    }

    v["NumCuePoints"] = juce::String (cue);
    v["NumCueLabels"] = juce::String (label);
    v["NumCueRegions"] = juce::String ((int) regions.size());
    return v;
}

void DataWindow::readCues (const juce::StringPairArray& meta)
{
    const auto total = (juce::int64) buffer.getNumSamples();
    std::map<int, juce::int64> offsets;
    std::map<int, juce::String> labels;

    for (int i = 0; i < meta["NumCuePoints"].getIntValue(); ++i)
    {
        const auto c = "Cue" + juce::String (i);
        offsets[meta[c + "Identifier"].getIntValue()] = meta[c + "Offset"].getLargeIntValue();
    }

    for (int i = 0; i < meta["NumCueLabels"].getIntValue(); ++i)
    {
        const auto l = "CueLabel" + juce::String (i);
        labels[meta[l + "Identifier"].getIntValue()] = meta[l + "Text"];
    }

    std::set<int> regionIds;

    for (int i = 0; i < meta["NumCueRegions"].getIntValue(); ++i)
    {
        const auto k = "CueRegion" + juce::String (i);
        const auto id = meta[k + "Identifier"].getIntValue();
        const auto length = meta[k + "SampleLength"].getLargeIntValue();

        if (offsets.count (id) == 0 || length <= 0)
            continue;

        regionIds.insert (id);
        const auto start = juce::jlimit ((juce::int64) 0, total, offsets[id]);
        const auto end = juce::jlimit ((juce::int64) 0, total, start + length);
        auto name = labels.count (id) != 0 && labels[id].isNotEmpty() ? labels[id] : meta[k + "Text"];

        if (name.isEmpty())
            name = "Region " + juce::String ((int) regions.size() + 1);

        if (end > start)
            regions.push_back ({ start, end, name });
    }

    for (const auto& [id, position] : offsets)
        if (regionIds.count (id) == 0 && position >= 0 && position <= total)
            markers.push_back (position);

    std::sort (markers.begin(), markers.end());
    markers.erase (std::unique (markers.begin(), markers.end()), markers.end());
    std::stable_sort (regions.begin(), regions.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
}

int DataWindow::addRegion (juce::Range<juce::int64> range, const juce::String& name)
{
    range = range.getIntersectionWith ({ 0, (juce::int64) buffer.getNumSamples() });

    if (range.isEmpty())
        return -1;

    pushUndo();
    regions.push_back ({ range.getStart(), range.getEnd(), name });
    std::stable_sort (regions.begin(), regions.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
    regionsChanged();

    for (int i = 0; i < (int) regions.size(); ++i)
        if (regions[(size_t) i].start == range.getStart() && regions[(size_t) i].end == range.getEnd() && regions[(size_t) i].name == name)
            return i;

    return -1;
}

void DataWindow::renameRegion (int index, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (index, (int) regions.size()) || regions[(size_t) index].name == name)
        return;

    pushUndo();
    regions[(size_t) index].name = name;
    regionsChanged();
}

void DataWindow::removeRegion (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) regions.size()))
        return;

    pushUndo();
    regions.erase (regions.begin() + index);
    regionsChanged();
}

void DataWindow::setRegions (std::vector<AudioRegion> newRegions)
{
    pushUndo();
    regions = std::move (newRegions);
    std::stable_sort (regions.begin(), regions.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
    regionsChanged();
}

juce::String DataWindow::nextRegionName() const
{
    int n = 1;

    for (const auto& r : regions)
        if (r.name.startsWith ("Region ") && r.name.substring (7).containsOnly ("0123456789"))
            n = std::max (n, r.name.substring (7).getIntValue() + 1);

    return "Region " + juce::String (n);
}

// Regions are part of the file (saved in WAV), so changing them is an edit.
void DataWindow::regionsChanged()
{
    modified = true;
    setName ("*" + title);
    view.setRegions (&regions);

    if (onChanged)
        onChanged();
}

void DataWindow::pruneBarStarts()
{
    std::vector<juce::int64> kept;

    for (auto b : barStarts)
        if (std::binary_search (markers.begin(), markers.end(), b))
            kept.push_back (b);

    barStarts = std::move (kept);
}

void DataWindow::pushUndo()
{
    redoStack.clear();
    undoStack.push_back ({ buffer, markers, barStarts, regions, sampleRate, bitDepth, resolutionBits, floatingPoint });

    size_t total = 0;
    for (const auto& b : undoStack)
        total += bytesOf (b);

    while (undoStack.size() > 1 && total > maxUndoBytes)
    {
        total -= bytesOf (undoStack.front());
        undoStack.erase (undoStack.begin());
    }
}

void DataWindow::restore (Snapshot& from, std::vector<Snapshot>& saveTo)
{
    saveTo.push_back ({ std::move (buffer), std::move (markers), std::move (barStarts), std::move (regions), sampleRate,
                        bitDepth, resolutionBits, floatingPoint });
    const auto channelsBefore = saveTo.back().audio.getNumChannels();
    buffer = std::move (from.audio);
    markers = std::move (from.markers);
    barStarts = std::move (from.barStarts);
    regions = std::move (from.regions);
    bitDepth = from.bitDepth;
    resolutionBits = from.resolutionBits;
    floatingPoint = from.floatingPoint;

    // Another rate or channel count (Resample, Channel Converter) needs a fresh view.
    if (! juce::approximatelyEqual (from.rate, sampleRate) || channelsBefore != buffer.getNumChannels())
    {
        sampleRate = from.rate;
        view.setAudio (&buffer, sampleRate);
    }

    audioChanged();
}

void DataWindow::undo()
{
    if (undoStack.empty())
        return;

    auto snapshot = std::move (undoStack.back());
    undoStack.pop_back();
    restore (snapshot, redoStack);
}

void DataWindow::redo()
{
    if (redoStack.empty())
        return;

    auto snapshot = std::move (redoStack.back());
    redoStack.pop_back();
    restore (snapshot, undoStack);
}

void DataWindow::audioChanged()
{
    view.setRegions (&regions);   // the strip shows / hides with them
    ++editCount;
    modified = true;
    setName ("*" + title);
    updateInfo();
    view.clearMarkerSelection();
    view.audioChanged();

    if (onChanged)
        onChanged();
}

namespace
{
constexpr int infoBarHeight = 22;
const juce::Colour infoBarColour (0xffd4d0c8);
}

void DataWindow::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (infoBarHeight);
    resolutionLabel.setBounds (top.reduced (4, 0));

    auto bottom = area.removeFromBottom (infoBarHeight).reduced (3, 2);
    cursorBox.setBounds (bottom.removeFromRight (170));
    bottom.removeFromRight (6);
    lengthBox.setBounds (bottom.removeFromRight (170));

    view.setBounds (area);
}

void DataWindow::paint (juce::Graphics& g)
{
    g.fillAll (infoBarColour);
}

void DataWindow::updateInfo()
{
    auto time = [this] (juce::int64 samples) { return WaveformView::formatTime ((double) samples / sampleRate); };

    resolutionLabel.setText (describeResolution(), juce::dontSendNotification);
    lengthBox.setText ("Length  " + time (buffer.getNumSamples()), juce::dontSendNotification);
    // While playing, the box follows the playhead in real time.
    const auto playhead = view.getPlayhead();
    cursorBox.setText ("Cursor  " + time (playhead >= 0 ? playhead : view.getCursor()), juce::dontSendNotification);
}

juce::String DataWindow::describeResolution() const
{
    const auto lossy = formatName.containsIgnoreCase ("MP3") || formatName.containsIgnoreCase ("Ogg")
                    || formatName.containsIgnoreCase ("Opus");
    const auto channels = buffer.getNumChannels();

    juce::String text;
    // 44100 -> "44,100"
    auto rate = juce::String (juce::roundToInt (sampleRate));
    for (int i = rate.length() - 3; i > 0; i -= 3)
        rate = rate.substring (0, i) + "," + rate.substring (i);

    text << rate << " Hz, ";

    if (lossy)
        text << formatName.upToFirstOccurrenceOf (" ", false, false) << ", ";
    else if (resolutionBits > 0)
        text << resolutionBits << "-bit (in 16-bit), ";
    else if (bitDepth > 0)
        text << bitDepth << "-bit" << (floatingPoint ? " float" : "") << ", ";

    text << (channels == 1 ? "Mono" : channels == 2 ? "Stereo" : juce::String (channels) + " channels");
    return text;
}

juce::String DataWindow::describeFormat() const
{
    // Compressed formats decode to float, so their "bit depth" is meaningless.
    const auto lossy = formatName.containsIgnoreCase ("MP3") || formatName.containsIgnoreCase ("Ogg")
                    || formatName.containsIgnoreCase ("Opus");
    const auto channels = buffer.getNumChannels();

    juce::String text;
    text << juce::String (juce::roundToInt (sampleRate)) << " Hz, "
         << (lossy ? formatName.upToFirstOccurrenceOf (" ", false, false) + ", "
                   : bitDepth > 0 ? juce::String (bitDepth) + "-bit, " : juce::String())
         << (channels == 1 ? "Mono" : channels == 2 ? "Stereo" : juce::String (channels) + " ch") << ", "
         << WaveformView::formatTime ((double) buffer.getNumSamples() / sampleRate);
    return text;
}
