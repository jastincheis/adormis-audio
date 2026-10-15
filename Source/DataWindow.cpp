#include "DataWindow.h"
#include "Mp3Encoder.h"

#if WAVEFORGE_HAS_SAMPLERATE
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
       #if WAVEFORGE_HAS_SAMPLERATE
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
    view.setMarkers (&markers);
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

            for (auto& m : markers)
                m = (juce::int64) std::llround ((double) m * targetRate / sampleRate);

            buffer = std::move (converted);
            sampleRate = targetRate;
            view.setAudio (&buffer, sampleRate);
        }

        file = destination;
        title = destination.getFileName();
        formatName = newFormatName;
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
    std::vector<juce::int64> kept;

    for (auto m : markers)
    {
        if (m < start)                    kept.push_back (m);
        else if (m >= start + removed)    kept.push_back (m - removed);
    }

    markers = std::move (kept);
    view.setSelection ({});
    view.setCursor (start);
    audioChanged();
    return true;
}

std::unique_ptr<DataWindow> DataWindow::createSilent (const juce::String& newTitle, double rate, int channels,
                                                      juce::int64 numSamples, std::vector<juce::int64> newMarkers)
{
    if (numSamples <= 0 || numSamples > std::numeric_limits<int>::max() || channels < 1)
        return nullptr;

    std::unique_ptr<DataWindow> doc (new DataWindow());
    doc->sampleRate = rate;
    doc->bitDepth = 16;
    doc->formatName = "WAV file";
    doc->buffer.setSize (channels, (int) numSamples);
    doc->buffer.clear();
    doc->markers = std::move (newMarkers);
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

void DataWindow::mixIn (const juce::AudioBuffer<float>& source, double sourceRate, juce::int64 position,
                        float sourceGain, float destGain)
{
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

    view.setSelection ({ start, start + length });
    audioChanged();
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

void DataWindow::removeMarker (int index)
{
    if (juce::isPositiveAndBelow (index, (int) markers.size()))
    {
        markers.erase (markers.begin() + index);
        view.clearMarkerSelection();
        view.repaint();
    }
}

void DataWindow::pushUndo()
{
    redoStack.clear();
    undoStack.push_back ({ buffer, markers, sampleRate });

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
    saveTo.push_back ({ std::move (buffer), std::move (markers), sampleRate });
    buffer = std::move (from.audio);
    markers = std::move (from.markers);

    if (! juce::approximatelyEqual (from.rate, sampleRate))
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
