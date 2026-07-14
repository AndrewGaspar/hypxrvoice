#include "AudioCapture.hpp"
#include "Clock.hpp"
#include "Log.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <atomic>
#include <cstring>
#include <vector>

// One-time PipeWire library init (pw_init is refcount-free; guard ourselves).
namespace {
    std::atomic<int> g_pwInit{0};
    void             ensurePwInit() {
        if (g_pwInit.fetch_add(1) == 0)
            pw_init(nullptr, nullptr);
    }
}

struct CAudioCapture::Impl {
    pw_thread_loop*          loop   = nullptr;
    pw_stream*               stream = nullptr;
    spa_hook                 streamListener{};
    CAudioCapture::Callback  cb;
    int                      sampleRate = 16000;
    uint32_t                 channels   = 1;
};

static void onProcess(void* userdata) {
    auto* impl = static_cast<CAudioCapture::Impl*>(userdata);
    pw_buffer* b = pw_stream_dequeue_buffer(impl->stream);
    if (!b)
        return;
    spa_buffer* buf = b->buffer;
    if (buf->n_datas > 0 && buf->datas[0].data) {
        auto&        d       = buf->datas[0];
        const float* samples = static_cast<const float*>(d.data);
        uint32_t     offset  = d.chunk->offset;
        uint32_t     size    = d.chunk->size;
        uint32_t     stride  = impl->channels * sizeof(float);
        if (stride > 0 && size >= stride) {
            const float* base = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(samples) + offset);
            uint32_t     nframes = size / stride;
            int64_t      t       = Clock::monotonicMs();
            if (impl->channels == 1) {
                if (impl->cb)
                    impl->cb(base, nframes, t);
            } else {
                // Downmix to mono (we request mono, but be defensive).
                std::vector<float> mono(nframes);
                for (uint32_t i = 0; i < nframes; i++) {
                    float acc = 0;
                    for (uint32_t c = 0; c < impl->channels; c++)
                        acc += base[i * impl->channels + c];
                    mono[i] = acc / impl->channels;
                }
                if (impl->cb)
                    impl->cb(mono.data(), nframes, t);
            }
        }
    }
    pw_stream_queue_buffer(impl->stream, b);
}

static const pw_stream_events s_streamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = onProcess,
};

CAudioCapture::~CAudioCapture() {
    stop();
}

bool CAudioCapture::start(const std::string& source, int sampleRate, Callback cb) {
    if (m_running) {
        if (source == m_source)
            return true;
        stop();
    }
    ensurePwInit();

    m_impl             = new Impl();
    m_impl->cb         = std::move(cb);
    m_impl->sampleRate = sampleRate;
    m_impl->channels   = 1;

    m_impl->loop = pw_thread_loop_new("hypxrvoice-capture", nullptr);
    if (!m_impl->loop) {
        Log::log(Log::ERR, "pw_thread_loop_new failed");
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_APP_NAME, "hypxrvoice",
        PW_KEY_NODE_NAME, "hypxrvoice-capture",
        nullptr);
    // A ".monitor" source names a SINK's monitor (the pulse-style convention, e.g.
    // "alsa_output.…​.monitor"). In native PipeWire a sink monitor is not a distinct
    // source node — it is captured by connecting to the SINK node with
    // stream.capture.sink=true — so translate the name and set that flag. This makes
    // desktop/played-audio capture work (the AEC seam above) and lets the loopback
    // integration test drive the real capture path through a null-sink monitor.
    std::string target = source;
    if (const std::string suffix = ".monitor";
        target.size() > suffix.size() && target.compare(target.size() - suffix.size(), suffix.size(), suffix) == 0) {
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        target.erase(target.size() - suffix.size()); // target the sink node itself
    }
    if (!target.empty())
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());

    pw_thread_loop_lock(m_impl->loop);

    m_impl->stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(m_impl->loop),
        "hypxrvoice-capture",
        props,
        &s_streamEvents,
        m_impl);
    if (!m_impl->stream) {
        Log::log(Log::ERR, "pw_stream_new_simple failed");
        pw_thread_loop_unlock(m_impl->loop);
        stop();
        return false;
    }

    uint8_t              buffer[1024];
    spa_pod_builder      bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw   info{};
    info.format   = SPA_AUDIO_FORMAT_F32;
    info.rate     = static_cast<uint32_t>(sampleRate);
    info.channels = 1;
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(
        m_impl->stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (res < 0) {
        Log::log(Log::ERR, "pw_stream_connect failed: {}", spa_strerror(res));
        pw_thread_loop_unlock(m_impl->loop);
        stop();
        return false;
    }

    pw_thread_loop_unlock(m_impl->loop);

    if (pw_thread_loop_start(m_impl->loop) < 0) {
        Log::log(Log::ERR, "pw_thread_loop_start failed");
        stop();
        return false;
    }

    m_running = true;
    m_source  = source;
    Log::log(Log::INFO, "audio capture started (source: '{}', {} Hz mono)", source.empty() ? "default" : source, sampleRate);
    return true;
}

void CAudioCapture::stop() {
    if (!m_impl)
        return;
    if (m_impl->loop)
        pw_thread_loop_stop(m_impl->loop);
    if (m_impl->stream) {
        pw_stream_destroy(m_impl->stream);
        m_impl->stream = nullptr;
    }
    if (m_impl->loop) {
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }
    delete m_impl;
    m_impl = nullptr;
    if (m_running)
        Log::log(Log::INFO, "audio capture stopped");
    m_running = false;
    m_source.clear();
}
