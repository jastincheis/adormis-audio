#include "StemSeparator.h"
#include "StartupLog.h"

#include "model.hpp"   // demucs.cpp

#include <atomic>
#include <thread>

namespace wf::stems
{
namespace
{
const char* modelName = "ggml-model-htdemucs-4s-f16.bin";

struct Cancelled {};

// The song is cut into `pieces` overlapping parts that run in parallel, then
// put back together with cross-fades: demucs.cpp's threaded_inference
// (cli-apps/threaded_inference.hpp, MIT), with cancelling and one progress.
Eigen::Tensor3dXf separateInPieces (const demucscpp::demucs_model& model, const Eigen::MatrixXf& audio, int pieces,
                                    std::vector<std::atomic<float>>& done, const std::atomic<bool>& cancel)
{
    const int overlap = (int) std::floor (modelRate * 0.75);
    const int total = (int) audio.cols();
    const int pieceLength = (int) std::ceil ((float) total / (float) pieces);

    std::vector<Eigen::MatrixXf> inputs;

    for (int i = 0; i < pieces; ++i)
    {
        const int start = i * pieceLength;
        const int end = std::min (total, start + pieceLength);
        Eigen::MatrixXf piece = Eigen::MatrixXf::Zero (2, end - start + 2 * overlap);

        if (i == 0)
            piece.block (0, 0, 2, overlap).colwise() = audio.col (0);
        else
            piece.block (0, 0, 2, overlap) = audio.block (0, start - overlap, 2, overlap);

        if (i == pieces - 1)
            piece.block (0, end - start + overlap, 2, total - end) = audio.block (0, end, 2, total - end);
        else
            piece.block (0, end - start + overlap, 2, overlap) = audio.block (0, end, 2, overlap);

        piece.block (0, overlap, 2, end - start) = audio.block (0, start, 2, end - start);
        inputs.push_back (piece);
    }

    std::vector<Eigen::Tensor3dXf> outputs ((size_t) pieces);
    std::vector<std::exception_ptr> failures ((size_t) pieces);
    std::vector<std::thread> threads;

    for (int i = 0; i < pieces; ++i)
        threads.emplace_back ([&, i]
        {
            try
            {
                outputs[(size_t) i] = demucscpp::demucs_inference (model, inputs[(size_t) i], [&, i] (float p, const std::string&)
                {
                    done[(size_t) i] = p;

                    if (cancel)
                        throw Cancelled {};
                });
            }
            catch (...)
            {
                failures[(size_t) i] = std::current_exception();
            }
        });

    for (auto& t : threads)
        t.join();

    for (auto& f : failures)
        if (f != nullptr)
            std::rethrow_exception (f);

    const int sources = model.is_4sources ? 4 : 6;
    Eigen::Tensor3dXf result (sources, 2, total);
    result.setZero();

    Eigen::VectorXf ramp (pieceLength);

    for (int i = 0; i < pieceLength; ++i)
        ramp (i) = (float) std::min (i + 1, pieceLength - i);

    ramp /= ramp.maxCoeff();
    Eigen::VectorXf weights = Eigen::VectorXf::Zero (total);

    for (int i = 0; i < pieces; ++i)
        for (int t = 0; t < sources; ++t)
            for (int ch = 0; ch < 2; ++ch)
                for (int j = 0; j < pieceLength + 2 * overlap; ++j)
                {
                    const int at = i * pieceLength + j - overlap;

                    if (at < 0 || at >= total || j >= (int) outputs[(size_t) i].dimension (2))
                        continue;

                    float w = 1.0f;

                    if (j < overlap)
                        w = ramp (j);
                    else if (j >= pieceLength)
                        w = ramp (pieceLength + 2 * overlap - j - 1);

                    result (t, ch, at) += outputs[(size_t) i] (t, ch, j) * w;
                    weights (at) += w;
                }

    // Each place was weighted once per source and channel.
    for (int t = 0; t < sources; ++t)
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < total; ++i)
                if (weights (i) > 0.0f)
                    result (t, ch, i) /= weights (i) / (2.0f * (float) sources);

    return result;
}
} // namespace

juce::File modelFile()
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    for (const auto& folder : { exe.getChildFile ("models"),                          // Windows, Linux
                                exe.getParentDirectory().getChildFile ("Resources/models"),   // macOS bundle
                                startup::settingsFolder().getChildFile ("models") })
        if (auto f = folder.getChildFile (modelName); f.existsAsFile())
            return f;

    return {};
}

int piecesAtOnce()
{
    const auto memoryGb = juce::SystemStats::getMemorySizeInMegabytes() / 1024;
    const auto cores = std::max (1, juce::SystemStats::getNumPhysicalCpus());
    const int byMemory = memoryGb < 8 ? 1 : memoryGb < 15 ? 2 : memoryGb < 24 ? 4 : 8;
    return std::max (1, std::min (byMemory, cores));
}

std::optional<std::array<juce::AudioBuffer<float>, numParts>> separate (const juce::AudioBuffer<float>& input,
                                                                         const std::function<bool (float)>& progress,
                                                                         juce::String& error)
{
    const auto file = modelFile();

    if (! file.existsAsFile())
    {
        error = "The AI model for stems (" + juce::String (modelName) + ") is missing from this copy of Adormis Audio.";
        return {};
    }

    if (input.getNumChannels() != 2 || input.getNumSamples() == 0)
    {
        error = "Stems need stereo audio.";
        return {};
    }

    auto model = std::make_unique<demucscpp::demucs_model>();

    if (! demucscpp::load_demucs_model (file.getFullPathName().toStdString(), model.get()))
    {
        error = "Could not load the AI model " + file.getFullPathName();
        return {};
    }

    const int total = input.getNumSamples();
    Eigen::MatrixXf audio (2, total);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < total; ++i)
            audio (ch, i) = input.getSample (ch, i);

    // Pieces of at least ~10 s (the model works on 7.8 s windows), so the joins stay few.
    const int pieces = std::max (1, std::min (piecesAtOnce(), (int) (total / (modelRate * 10.0))));
    startup::log ("stems: " + juce::String (total / modelRate, 1) + " s in " + juce::String (pieces) + " pieces");

    std::vector<std::atomic<float>> done ((size_t) pieces);
    std::atomic<bool> cancel { false }, finished { false };
    std::exception_ptr failure;
    Eigen::Tensor3dXf parts;

    std::thread worker ([&]
    {
        try
        {
            parts = separateInPieces (*model, audio, pieces, done, cancel);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        finished = true;
    });

    // The progress (and Cancel) is asked from this thread only.
    for (;;)
    {
        float sum = 0.0f;

        for (auto& d : done)
            sum += d;

        if (progress != nullptr && ! progress (sum / (float) pieces))
            cancel = true;

        if (finished || cancel)
            break;

        juce::Thread::sleep (150);
    }

    worker.join();

    if (cancel)
        return {};

    if (failure != nullptr)
    {
        try { std::rethrow_exception (failure); }
        catch (const std::exception& e) { error = "Stems failed: " + juce::String (e.what()); }
        catch (...) { error = "Stems failed."; }
        return {};
    }

    std::array<juce::AudioBuffer<float>, numParts> result;

    for (int p = 0; p < numParts; ++p)
    {
        result[(size_t) p].setSize (2, total);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto* out = result[(size_t) p].getWritePointer (ch);

            for (int i = 0; i < total; ++i)
                out[i] = parts (p, ch, i);
        }
    }

    return result;
}
} // namespace wf::stems
