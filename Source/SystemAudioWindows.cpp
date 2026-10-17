// Windows: WASAPI loopback of the default output. While it records, a silent
// stream plays on the same output, so the loopback keeps delivering (silence
// included) when nothing else plays and the recording keeps its timing.

#include "SystemAudioBackend.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#pragma comment (lib, "ole32.lib")

namespace wf
{
namespace
{
template <typename T>
void release (T*& p)
{
    if (p != nullptr)
        p->Release();

    p = nullptr;
}

std::string toUtf8 (const wchar_t* text)
{
    if (text == nullptr)
        return {};

    const auto size = WideCharToMultiByte (CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out ((size_t) std::max (0, size - 1), '\0');

    if (size > 1)
        WideCharToMultiByte (CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);

    return out;
}

class WasapiLoopback final : public SystemAudioBackend
{
public:
    ~WasapiLoopback() override   { stop(); }

    bool start (Sink sinkIn, std::string& error) override
    {
        stop();
        sink = std::move (sinkIn);
        running = true;
        std::promise<std::string> ready;
        auto result = ready.get_future();
        thread = std::thread ([this, &ready] { run (ready); });

        if (result.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        {
            error = "The computer's audio output did not answer.";
            stop();
            return false;
        }

        error = result.get();

        if (! error.empty())
        {
            stop();
            return false;
        }

        return true;
    }

    void stop() override
    {
        running = false;

        if (thread.joinable())
            thread.join();
    }

    double sampleRate() const override   { return rate.load(); }
    std::string name() const override    { return deviceName; }

private:
    void run (std::promise<std::string>& ready)
    {
        const auto comResult = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IPropertyStore* properties = nullptr;
        IAudioClient* capture = nullptr;
        IAudioClient* render = nullptr;
        IAudioCaptureClient* captureClient = nullptr;
        IAudioRenderClient* renderClient = nullptr;
        WAVEFORMATEX* format = nullptr;
        bool told = false;

        auto fail = [&] (const char* what, HRESULT hr)
        {
            char code[16];
            wsprintfA (code, "%08lX", (unsigned long) hr);
            ready.set_value (std::string ("Could not record what the computer plays (") + what + ", error 0x" + code + ").");
            told = true;
        };

        do
        {
            HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator),
                                           (void**) &enumerator);
            if (FAILED (hr)) { fail ("no audio devices", hr); break; }

            hr = enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device);
            if (FAILED (hr)) { fail ("no default output", hr); break; }

            if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &properties)))
            {
                PROPVARIANT value;
                PropVariantInit (&value);

                if (SUCCEEDED (properties->GetValue (PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                    deviceName = toUtf8 (value.pwszVal);

                PropVariantClear (&value);
            }

            if (deviceName.empty())
                deviceName = "the default output";

            hr = device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &capture);
            if (FAILED (hr)) { fail ("output not available", hr); break; }

            hr = capture->GetMixFormat (&format);
            if (FAILED (hr)) { fail ("unknown output format", hr); break; }

            auto isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;

            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (format)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;

            const auto channels = (int) format->nChannels;
            const auto bits = (int) format->wBitsPerSample;
            rate = (double) format->nSamplesPerSec;

            if (channels < 1 || (isFloat && bits != 32) || (! isFloat && bits != 16 && bits != 24 && bits != 32))
            {
                fail ("unsupported output format", E_FAIL);
                break;
            }

            const REFERENCE_TIME second = 10000000;
            hr = capture->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, second, 0, format, nullptr);
            if (FAILED (hr)) { fail ("loopback refused", hr); break; }

            hr = capture->GetService (__uuidof (IAudioCaptureClient), (void**) &captureClient);
            if (FAILED (hr)) { fail ("loopback refused", hr); break; }

            // The silent stream that keeps the output (and so the loopback) running.
            UINT32 renderFrames = 0;

            if (SUCCEEDED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &render))
                && SUCCEEDED (render->Initialize (AUDCLNT_SHAREMODE_SHARED, 0, second, 0, format, nullptr))
                && SUCCEEDED (render->GetBufferSize (&renderFrames))
                && SUCCEEDED (render->GetService (__uuidof (IAudioRenderClient), (void**) &renderClient)))
            {
                BYTE* data = nullptr;

                if (SUCCEEDED (renderClient->GetBuffer (renderFrames, &data)))
                    renderClient->ReleaseBuffer (renderFrames, AUDCLNT_BUFFERFLAGS_SILENT);

                render->Start();
            }
            else
            {
                release (renderClient);
                release (render);
            }

            hr = capture->Start();
            if (FAILED (hr)) { fail ("loopback did not start", hr); break; }

            ready.set_value ({});
            told = true;

            std::vector<float> left, right;

            while (running.load())
            {
                Sleep (10);

                if (renderClient != nullptr)
                {
                    UINT32 padding = 0;
                    BYTE* data = nullptr;

                    if (SUCCEEDED (render->GetCurrentPadding (&padding)) && padding < renderFrames
                        && SUCCEEDED (renderClient->GetBuffer (renderFrames - padding, &data)))
                        renderClient->ReleaseBuffer (renderFrames - padding, AUDCLNT_BUFFERFLAGS_SILENT);
                }

                UINT32 packet = 0;

                while (running.load() && SUCCEEDED (hr = captureClient->GetNextPacketSize (&packet)) && packet > 0)
                {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;

                    if (FAILED (hr = captureClient->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                        break;

                    left.assign (frames, 0.0f);
                    right.assign (frames, 0.0f);

                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0)
                    {
                        const auto rightChannel = channels > 1 ? 1 : 0;

                        for (UINT32 i = 0; i < frames; ++i)
                        {
                            const auto at = [&] (int c) -> float
                            {
                                const auto index = (size_t) i * (size_t) channels + (size_t) c;

                                if (isFloat)    return reinterpret_cast<const float*> (data)[index];
                                if (bits == 16) return reinterpret_cast<const short*> (data)[index] / 32768.0f;
                                if (bits == 32) return (float) (reinterpret_cast<const int*> (data)[index] / 2147483648.0);

                                const auto* b = data + index * 3;   // 24-bit packed
                                const auto v = (int) ((unsigned) b[0] << 8 | (unsigned) b[1] << 16 | (unsigned) b[2] << 24) >> 8;
                                return v / 8388608.0f;
                            };

                            left[i] = at (0);
                            right[i] = at (rightChannel);
                        }
                    }

                    captureClient->ReleaseBuffer (frames);
                    const float* both[] { left.data(), right.data() };
                    sink (both, (int) frames);
                }

                if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                    break;   // the output went away (unplugged, switched): the recording just stops
            }

            capture->Stop();

            if (render != nullptr)
                render->Stop();
        }
        while (false);

        if (! told)
            ready.set_value ("Could not record what the computer plays.");

        release (renderClient);
        release (captureClient);
        release (render);
        release (capture);
        release (properties);
        release (device);
        release (enumerator);

        if (format != nullptr)
            CoTaskMemFree (format);

        if (SUCCEEDED (comResult))
            CoUninitialize();
    }

    Sink sink;
    std::atomic<bool> running { false };
    std::atomic<double> rate { 48000.0 };
    std::string deviceName;
    std::thread thread;
};
} // namespace

std::unique_ptr<SystemAudioBackend> createSystemAudioBackend (const char*)
{
    return std::make_unique<WasapiLoopback>();
}
} // namespace wf
