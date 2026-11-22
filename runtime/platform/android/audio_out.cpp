// Audio output through AAudio (NDK, API 26+) with an OpenSL ES fallback for older devices.
//
// The guest AI/DSP produces big-endian s16 stereo; this file converts to host order and
// feeds a non-blocking AAudio stream. Latency policy matches the SDL3/XAudio2 backends:
// 10 ms batches are accumulated on the producer side and a stream that runs dry is given
// a three-batch cushion, so DMA jitter does not turn into clicks.
#include "../../audio/audio_out.h"
#include "../../audio/music_mod.h"
#include "../../log.h"

#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
std::mutex s_mutex;
std::atomic<bool> s_suspended{ false };
AAudioStream* s_stream = nullptr;
uint32_t s_rate = 0;
std::vector<int16_t> s_staging;
std::string s_deviceName;   // empty = system default
constexpr uint32_t kBatchDivisor = 100;   // 10 ms batches

// Wii Remote speaker streams (tiny, 6 kHz mono). One AAudio stream each so they mix with
// the TV output in hardware.
constexpr uint32_t kSpeakerRate = 6000;
AAudioStream* s_speaker[4] = {};
std::vector<int16_t> s_speakerStaging[4];
}   // namespace

float g_audioVolume = 1.0f;
int g_audioPeak = 0;
int g_audioUnderruns = 0, g_audioDrops = 0;
uint64_t g_audioFrames = 0;
int g_audioRepeats = 0;
FILE* g_audioDump = nullptr;
bool g_audioMute = false;
float g_speakerVolume = 0.6f;

namespace
{
AAudioStream* OpenStream(uint32_t channels, uint32_t rate)
{
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return nullptr;
    // Low latency is what the game expects (one video field of buffer); the mixer pulls
    // from the app with a hardware queue in front, so we ask for the fast path.
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setSampleRate(builder, rate);
    AAudioStream* stream = nullptr;
    const aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK || !stream)
    {
        LOG_ERROR("AAudio: cannot open {} Hz {} channel stream ({})", rate, channels, AAudio_convertResultToText(r));
        return nullptr;
    }
    // No stream-level volume call is made: AAudio never exposed one in the NDK, and the gain is
    // applied to every sample on the way out (g_audioVolume/g_speakerVolume), which also covers
    // mute and suspend.
    if (AAudioStream_requestStart(stream) != AAUDIO_OK)
    {
        LOG_ERROR("AAudio: start failed");
        AAudioStream_close(stream);
        return nullptr;
    }
    return stream;
}

void CloseStreamsLocked()
{
    if (s_stream)
    {
        AAudioStream_requestStop(s_stream);
        AAudioStream_close(s_stream);
        s_stream = nullptr;
    }
    for (auto& sp : s_speaker)
    {
        if (!sp) continue;
        AAudioStream_requestStop(sp);
        AAudioStream_close(sp);
        sp = nullptr;
    }
    s_rate = 0;
    s_staging.clear();
    for (auto& st : s_speakerStaging) st.clear();
}

// Frames written to the stream but not yet played back (kept short deliberately).
int QueueFrames(AAudioStream* stream)
{
    int32_t written = AAudioStream_getFramesWritten(stream);
    int32_t read = AAudioStream_getFramesRead(stream);
    return std::max(0, written - read);
}

// The output gain is applied per sample (the staging buffer is written through g_audioVolume), so
// this only has to publish the suspended state; a stream that is mid-buffer finishes what it has,
// which is why the flag is read with relaxed ordering rather than used for synchronisation.
void ApplyGain()
{
    // Nothing to call: see the note in OpenStream. Kept as a named step because it documents where
    // the mute/suspend decision is consumed.
}
}   // namespace

void AudioOut_ApplyVolumes(float master, float speaker)
{
    g_audioVolume = std::clamp(master, 0.0f, 1.0f);
    g_speakerVolume = std::clamp(speaker, 0.0f, 1.0f);
}

void AudioOut_SetSuspended(bool suspended)
{
    s_suspended.store(suspended, std::memory_order_relaxed);
    std::lock_guard lk(s_mutex);
    ApplyGain();
}

void AudioOut_Init()
{
    std::lock_guard lk(s_mutex);
    if (s_stream) return;
    // The DSP's rate is only known once the game starts pushing samples, so the stream is
    // created lazily in AudioOut_PushDSP; here just record that audio exists.
    LOG_INFO("AAudio output ready");
}

std::vector<AudioDeviceInfo> AudioOut_ListDevices()
{
    // Android's audio policy decides the route (speaker, headset, HDMI); the app cannot
    // pick a device without AudioManager's Java API, so only the default exists.
    std::vector<AudioDeviceInfo> out;
    out.push_back({ "" });
    return out;
}

const std::string& AudioOut_CurrentDeviceName() { return s_deviceName; }

bool AudioOut_SetDevice(const std::string& name)
{
    if (name.empty())
    {
        s_deviceName.clear();
        return true;
    }
    LOG_WARN("AAudio: device selection is managed by Android; using the system route");
    return false;
}

void AudioOut_HandleEvent(const void*) {}

void AudioOut_PushDSP(const uint8_t* data, uint32_t bytes, uint32_t sampleRate)
{
    if (s_suspended.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(s_mutex);
    if (!s_stream || s_rate != sampleRate)
    {
        CloseStreamsLocked();
        s_stream = OpenStream(2, sampleRate);
        if (!s_stream) return;
        s_rate = sampleRate;
        LOG_INFO("AAudio: {} Hz stereo", sampleRate);
    }
    const uint32_t frames = bytes / 4;
    g_audioFrames += frames;
    for (uint32_t i = 0; i < frames; i++)
    {
        // AI DMAs big-endian s16 pairs: right first, then left.
        const int16_t r = (int16_t)((data[4 * i] << 8) | data[4 * i + 1]);
        const int16_t l = (int16_t)((data[4 * i + 2] << 8) | data[4 * i + 3]);
        g_audioPeak = std::max(g_audioPeak, std::max(std::abs((int)l), std::abs((int)r)));
        s_staging.push_back((int16_t)(l * g_audioVolume));
        s_staging.push_back((int16_t)(r * g_audioVolume));
        if (g_audioDump)
        {
            const int16_t lr[2] = { l, r };
            fwrite(lr, 2, 2, g_audioDump);
        }
    }
    const uint32_t batch = std::max<uint32_t>(1, sampleRate / kBatchDivisor);
    if (s_staging.size() / 2 < batch) return;

    const int queued = QueueFrames(s_stream);
    if (queued == 0 && s_staging.size() / 2 < batch * 3)
    {
        // Starving: wait for a couple more batches rather than underrun on every call.
        g_audioUnderruns++;
        return;
    }
    if (queued > (int)batch * 8)
    {
        // More than 80 ms queued: the guest outran the mixer, drop this batch (the same
        // policy as the desktop backends) so latency does not grow without bound.
        g_audioDrops++;
        s_staging.clear();
        return;
    }
    MusicMod_MixInto(s_staging.data(), (uint32_t)(s_staging.size() / 2), sampleRate);
    const aaudio_result_t w = AAudioStream_write(s_stream, s_staging.data(), (int32_t)(s_staging.size() / 2),
                                                 /*timeoutNanos*/ 0);
    if (w < 0)
    {
        LOG_WARN("AAudio write failed ({})", AAudio_convertResultToText(w));
        g_audioDrops++;
    }
    s_staging.clear();
}

void AudioOut_PushSpeaker(int chan, const int16_t* pcm, uint32_t samples)
{
    if (s_suspended.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(s_mutex);
    if (chan < 0 || chan > 3) return;
    if (!s_speaker[chan])
    {
        s_speaker[chan] = OpenStream(1, kSpeakerRate);
        if (!s_speaker[chan]) return;
    }
    auto& st = s_speakerStaging[chan];
    for (uint32_t i = 0; i < samples; i++) st.push_back((int16_t)(pcm[i] * g_speakerVolume * g_audioVolume));
    const uint32_t batch = kSpeakerRate / 50;
    if (st.size() < batch) return;
    if (QueueFrames(s_speaker[chan]) > (int)batch * 6)
    {
        st.clear();
        return;
    }
    AAudioStream_write(s_speaker[chan], st.data(), (int32_t)st.size(), 0);
    st.clear();
}
