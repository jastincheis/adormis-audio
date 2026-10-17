// Linux: the monitor of the default output, through libpulse-simple (which
// PipeWire also provides). The library is loaded when needed, so the app
// still starts on systems without it.

#include "SystemAudioBackend.h"
#include <atomic>
#include <cstdint>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace wf
{
namespace
{
struct PaSampleSpec { int format; std::uint32_t rate; std::uint8_t channels; };
struct PaBufferAttr { std::uint32_t maxlength, tlength, prebuf, minreq, fragsize; };
constexpr int paSampleFloat32LE = 5, paStreamRecord = 2;

using PaNew = void* (*) (const char*, const char*, int, const char*, const char*, const PaSampleSpec*, const void*, const PaBufferAttr*, int*);
using PaRead = int (*) (void*, void*, std::size_t, int*);
using PaFree = void (*) (void*);
using PaError = const char* (*) (int);

class PulseMonitor final : public SystemAudioBackend
{
public:
    explicit PulseMonitor (const char* deviceIn) : device (deviceIn != nullptr ? deviceIn : "@DEFAULT_MONITOR@") {}
    ~PulseMonitor() override { stop(); if (library != nullptr) dlclose (library); }

    bool start (Sink sinkIn, std::string& error) override
    {
        stop();

        if (library == nullptr)
            library = dlopen ("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);

        if (library == nullptr)
        {
            error = "Recording what the computer plays needs PulseAudio or PipeWire (libpulse-simple).";
            return false;
        }

        paNew = (PaNew) dlsym (library, "pa_simple_new");
        paRead = (PaRead) dlsym (library, "pa_simple_read");
        paFree = (PaFree) dlsym (library, "pa_simple_free");
        auto* core = dlopen ("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
        const auto paError = core != nullptr ? (PaError) dlsym (core, "pa_strerror") : nullptr;

        if (paNew == nullptr || paRead == nullptr || paFree == nullptr)
        {
            error = "The PulseAudio library on this system is incomplete.";
            return false;
        }

        const PaSampleSpec spec { paSampleFloat32LE, (std::uint32_t) rate, 2 };
        const auto chunk = (std::uint32_t) (rate / 100) * 8;   // 10 ms of stereo float
        const PaBufferAttr attr { (std::uint32_t) -1, (std::uint32_t) -1, (std::uint32_t) -1, (std::uint32_t) -1, chunk };
        int code = 0;
        stream = paNew (nullptr, "Adormis Audio", paStreamRecord, device.c_str(), "Record what the computer plays", &spec, nullptr, &attr, &code);

        if (stream == nullptr)
        {
            error = std::string ("Could not listen to the computer's output: ") + (paError != nullptr ? paError (code) : "unknown error");
            return false;
        }

        sink = std::move (sinkIn);
        running = true;
        thread = std::thread ([this, chunk]
        {
            std::vector<float> interleaved (chunk / 4), left (chunk / 8), right (chunk / 8);
            const float* channels[] { left.data(), right.data() };
            int err = 0;

            while (running.load())
            {
                if (paRead (stream, interleaved.data(), chunk, &err) < 0)
                    break;

                for (std::size_t i = 0; i < left.size(); ++i)
                {
                    left[i] = interleaved[2 * i];
                    right[i] = interleaved[2 * i + 1];
                }

                sink (channels, (int) left.size());
            }
        });

        return true;
    }

    void stop() override
    {
        running = false;

        if (thread.joinable())
            thread.join();   // a read returns within 10 ms

        if (stream != nullptr)
        {
            paFree (stream);
            stream = nullptr;
        }
    }

    double sampleRate() const override   { return rate; }
    std::string name() const override    { return device == "@DEFAULT_MONITOR@" ? "the default output" : device; }

private:
    static constexpr double rate = 48000.0;
    std::string device;
    void* library = nullptr;
    void* stream = nullptr;
    PaNew paNew = nullptr;
    PaRead paRead = nullptr;
    PaFree paFree = nullptr;
    Sink sink;
    std::atomic<bool> running { false };
    std::thread thread;
};
} // namespace

std::unique_ptr<SystemAudioBackend> createSystemAudioBackend (const char* device)
{
    return std::make_unique<PulseMonitor> (device);
}
} // namespace wf
