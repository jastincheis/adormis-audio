#pragma once

// Platform side of "record what the computer plays" (no JUCE here, so the
// Windows / macOS system headers stay out of the JUCE translation units).
// Each backend captures the default output, mixed from every program
// (WASAPI loopback, ScreenCaptureKit, the PulseAudio / PipeWire monitor).

#include <functional>
#include <memory>
#include <string>

namespace wf
{
class SystemAudioBackend
{
public:
    // Two channels of float samples, called from the backend's own thread.
    using Sink = std::function<void (const float* const* channels, int numSamples)>;

    virtual ~SystemAudioBackend() = default;

    // Starts capturing; false with a message for the user when it can't.
    virtual bool start (Sink sink, std::string& error) = 0;
    virtual void stop() = 0;

    virtual double sampleRate() const = 0;
    virtual std::string name() const = 0;   // the output being recorded
};

// Empty where the system has no way to do it. `device` (Linux only, for
// tests) names a monitor source instead of the default one.
std::unique_ptr<SystemAudioBackend> createSystemAudioBackend (const char* device = nullptr);
} // namespace wf
