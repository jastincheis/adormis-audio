// macOS: ScreenCaptureKit's audio capture (macOS 13 or newer) of everything
// the Mac plays. It needs the Screen & System Audio Recording permission;
// no picture is taken (the video side is set to 2 x 2 pixels, once a second,
// and ignored).

#include "SystemAudioBackend.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <vector>

API_AVAILABLE (macos (13.0))
@interface AdormisSystemAudioTap : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
    wf::SystemAudioBackend::Sink* sink;
    std::vector<float> left, right;
}
@end

@implementation AdormisSystemAudioTap

- (void) stream: (SCStream*) stream didOutputSampleBuffer: (CMSampleBufferRef) buffer ofType: (SCStreamOutputType) type
{
    if (type != SCStreamOutputTypeAudio || sink == nullptr || ! CMSampleBufferIsValid (buffer))
        return;

    const auto* description = CMAudioFormatDescriptionGetStreamBasicDescription (CMSampleBufferGetFormatDescription (buffer));

    if (description == nullptr || (description->mFormatFlags & kAudioFormatFlagIsFloat) == 0 || description->mBitsPerChannel != 32)
        return;

    struct { AudioBufferList list; AudioBuffer more[7]; } buffers;
    CMBlockBufferRef block = nullptr;

    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (buffer, nullptr, &buffers.list, sizeof (buffers), nullptr, nullptr,
                                                                 kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block) != noErr)
        return;

    const auto frames = (int) CMSampleBufferGetNumSamples (buffer);
    const auto channels = (int) description->mChannelsPerFrame;
    const auto planar = (description->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    left.resize ((size_t) frames);
    right.resize ((size_t) frames);

    for (int i = 0; i < frames; ++i)
    {
        if (planar)
        {
            const auto count = (int) buffers.list.mNumberBuffers;
            left[(size_t) i] = static_cast<const float*> (buffers.list.mBuffers[0].mData)[i];
            right[(size_t) i] = static_cast<const float*> (buffers.list.mBuffers[count > 1 ? 1 : 0].mData)[i];
        }
        else
        {
            const auto* data = static_cast<const float*> (buffers.list.mBuffers[0].mData);
            left[(size_t) i] = data[i * channels];
            right[(size_t) i] = data[i * channels + (channels > 1 ? 1 : 0)];
        }
    }

    CFRelease (block);
    const float* both[] { left.data(), right.data() };
    (*sink) (both, frames);
}

- (void) stream: (SCStream*) stream didStopWithError: (NSError*) error
{
    // the capture ended on its own (permission taken back, display gone): the take just ends
}

@end

namespace wf
{
namespace
{
class ScreenCaptureAudio final : public SystemAudioBackend
{
public:
    ~ScreenCaptureAudio() override   { stop(); }

    bool start (Sink sinkIn, std::string& error) override
    {
        stop();

        if (@available (macOS 13.0, *))
        {
            if (! CGPreflightScreenCaptureAccess())
            {
                CGRequestScreenCaptureAccess();
                error = "Allow Adormis Audio in System Settings > Privacy & Security > Screen & System Audio Recording, "
                        "then quit and open Adormis Audio again.";
                return false;
            }

            sink = std::move (sinkIn);
            __block SCShareableContent* content = nil;
            __block NSError* failure = nil;
            auto done = dispatch_semaphore_create (0);

            [SCShareableContent getShareableContentWithCompletionHandler: ^(SCShareableContent* c, NSError* e)
            {
                content = c;
                failure = e;
                dispatch_semaphore_signal (done);
            }];

            if (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0
                || content == nil || content.displays.count == 0)
            {
                error = "Could not record what the Mac plays"
                        + (failure != nil ? std::string (": ") + failure.localizedDescription.UTF8String : std::string ("."));
                return false;
            }

            auto* filter = [[SCContentFilter alloc] initWithDisplay: content.displays.firstObject excludingWindows: @[]];
            auto* config = [[SCStreamConfiguration alloc] init];
            config.capturesAudio = YES;
            config.excludesCurrentProcessAudio = NO;
            config.sampleRate = 48000;
            config.channelCount = 2;
            config.width = 2;
            config.height = 2;
            config.minimumFrameInterval = CMTimeMake (1, 1);
            config.showsCursor = NO;

            auto* newTap = [[AdormisSystemAudioTap alloc] init];
            newTap->sink = &sink;
            queue = dispatch_queue_create ("Adormis Audio system audio", DISPATCH_QUEUE_SERIAL);
            auto* newStream = [[SCStream alloc] initWithFilter: filter configuration: config delegate: newTap];
            NSError* addError = nil;

            if (! [newStream addStreamOutput: newTap type: SCStreamOutputTypeAudio sampleHandlerQueue: queue error: &addError]
                || ! [newStream addStreamOutput: newTap type: SCStreamOutputTypeScreen sampleHandlerQueue: queue error: &addError])
            {
                error = std::string ("Could not record what the Mac plays: ") + addError.localizedDescription.UTF8String;
                return false;
            }

            __block NSError* startError = nil;
            done = dispatch_semaphore_create (0);

            [newStream startCaptureWithCompletionHandler: ^(NSError* e)
            {
                startError = e;
                dispatch_semaphore_signal (done);
            }];

            if (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0 || startError != nil)
            {
                error = "Could not record what the Mac plays"
                        + (startError != nil ? std::string (": ") + startError.localizedDescription.UTF8String : std::string ("."));
                newTap->sink = nullptr;
                return false;
            }

            tap = newTap;
            stream = newStream;
            return true;
        }

        error = "Recording what the Mac plays needs macOS 13 (Ventura) or newer.";
        return false;
    }

    void stop() override
    {
        if (@available (macOS 13.0, *))
        {
            if (stream == nil)
                return;

            auto done = dispatch_semaphore_create (0);

            [(SCStream*) stream stopCaptureWithCompletionHandler: ^(NSError*) { dispatch_semaphore_signal (done); }];
            dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));

            // let a buffer that is still being handed over finish, then cut the sink off
            dispatch_sync (queue, ^{});
            ((AdormisSystemAudioTap*) tap)->sink = nullptr;
            stream = nil;
            tap = nil;
        }
    }

    double sampleRate() const override   { return 48000.0; }
    std::string name() const override    { return "this Mac"; }

private:
    Sink sink;
    id stream = nil, tap = nil;
    dispatch_queue_t queue = nullptr;
};
} // namespace

std::unique_ptr<SystemAudioBackend> createSystemAudioBackend (const char*)
{
    return std::make_unique<ScreenCaptureAudio>();
}
} // namespace wf
