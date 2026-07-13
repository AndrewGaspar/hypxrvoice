#include "Daemon.hpp"
#include "Clock.hpp"
#include "DesktopContext.hpp"
#include "Executor.hpp"
#include "Feedback.hpp"
#include "GazeResolver.hpp"
#include "IntentPipeline.hpp"
#include "Log.hpp"
#include "Pipeline.hpp"

#include <cstdint>
#include <cstring>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

CDaemon::~CDaemon() {
    m_audio.stop();
    m_control.stop();
    if (m_eventFd >= 0) close(m_eventFd);
    if (m_timerFd >= 0) close(m_timerFd);
    if (m_epollFd >= 0) close(m_epollFd);
}

void CDaemon::resetVad() {
    SVadConfig vc;
    vc.sampleRate      = m_cfg.audio.sampleRate;
    vc.energyThreshold = m_cfg.vad.energyThreshold;
    vc.startMs         = m_cfg.vad.startMs;
    vc.endMs           = m_cfg.vad.endMs;
    vc.maxUtteranceMs  = m_cfg.vad.maxUtteranceMs;
    vc.preRollMs       = m_cfg.vad.preRollMs;
    m_vad              = std::make_unique<CVad>(vc);
}

bool CDaemon::init(const std::string& configPath, std::string& err) {
    m_configPath = configPath;
    std::vector<std::string> errors, warnings;
    if (!loadConfigFile(configPath, m_cfg, errors, warnings)) {
        err = "config parse failed: " + (errors.empty() ? "" : errors.front());
        return false;
    }
    for (auto& w : warnings)
        Log::log(Log::WARN, "config: {}", w);

    m_machine.configure(m_cfg.activation.mode, m_cfg.activation.fallback);
    resetVad();

    // ASR model load is non-fatal: the daemon still runs (control/status work) so
    // the user can fetch a model and `reload`. Transcription is a no-op until then.
    std::string asrErr;
    CAsr::SParams ap{m_cfg.asr.model, m_cfg.asr.language, m_cfg.asr.threads, m_cfg.asr.translate};
    if (!m_asr.load(ap, asrErr))
        Log::log(Log::ERR, "ASR unavailable: {}", asrErr);

    m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    m_timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    m_epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (m_eventFd < 0 || m_timerFd < 0 || m_epollFd < 0) {
        err = "failed to create epoll/eventfd/timerfd";
        return false;
    }
    // ~4 Hz tick.
    itimerspec its{};
    its.it_interval.tv_nsec = 250'000'000;
    its.it_value.tv_nsec    = 250'000'000;
    timerfd_settime(m_timerFd, 0, &its, nullptr);

    if (!m_control.start([this](const std::string& c) { return handleControl(c); }, err))
        return false;

    auto add = [&](int fd) {
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev);
    };
    add(m_control.fd());
    add(m_eventFd);
    add(m_timerFd);

    // Prime the environment + mic policy immediately.
    m_env = m_compositor.poll(&m_lastRawStatus);
    m_machine.onEnv(m_env);
    applyMicPolicy();
    Log::log(Log::INFO, "hypxrvoiced ready — state {}, mode {}", stateName(m_machine.state()),
             m_cfg.activation.mode == EActivationMode::Auto ? "auto" : (m_cfg.activation.mode == EActivationMode::Ptt ? "ptt" : "wake"));
    return true;
}

void CDaemon::onAudio(const float* frames, size_t n, int64_t monoMs) {
    {
        std::lock_guard<std::mutex> lk(m_qMu);
        m_queue.push_back(SChunk{std::vector<float>(frames, frames + n), monoMs});
    }
    uint64_t one = 1;
    (void)!write(m_eventFd, &one, sizeof(one));
}

void CDaemon::drainAudio() {
    std::deque<SChunk> local;
    {
        std::lock_guard<std::mutex> lk(m_qMu);
        local.swap(m_queue);
    }
    if (!m_vad)
        return;
    std::vector<SSpeechSegment> segs;
    for (auto& c : local)
        m_vad->push(c.samples.data(), c.samples.size(), c.monoMs, segs);
    for (auto& s : segs)
        handleSegment(s);
}

void CDaemon::handleSegment(const SSpeechSegment& seg) {
    const bool pttWindow = m_machine.state() == EState::Listening && m_machine.currentActivation() == EActivation::PushToTalk;
    const bool requireWake = !pttWindow;
    const EActivation act = pttWindow ? EActivation::PushToTalk : EActivation::WakeWord;

    if (!m_asr.loaded()) {
        Log::log(Log::WARN, "dropping {}ms segment — no ASR model loaded", seg.endMs - seg.onsetMs);
        return;
    }

    STranscript t;
    if (!Pipeline::processSegment(m_asr, m_cfg, seg, act, requireWake, t)) {
        Log::log(Log::DEBUG, "segment rejected (onset {}ms): not addressed / empty", seg.onsetMs);
        return;
    }
    m_lastText    = t.text;
    m_lastOnsetMs = t.onsetMs;
    Feedback::emitTranscript(t, m_cfg);

    // WP-V4 intent tier: transcript -> context snapshot -> command -> executor. Uses
    // the live compositor for the read-only snapshot + gaze ring, and the executor's
    // (default dry-run) actuator. All injected here so the pipeline stays testable.
    if (m_cfg.intent.enabled) {
        auto r = IntentPipeline::process(t, m_cfg, defaultHyprctlQuery, defaultGazeQuery,
                                         defaultRunner);
        m_lastAction = std::string(verbName(r.action.verb)) +
                       (r.action.target.empty() ? "" : " " + r.action.target);
    }
}

std::string CDaemon::chooseSource() const {
    // In the headset, prefer the headset mic (wivrn.source); otherwise the configured
    // desk source (empty => PipeWire default).
    if (m_env.headsetPresent && !m_cfg.audio.headsetSource.empty())
        return m_cfg.audio.headsetSource;
    return m_cfg.audio.source;
}

void CDaemon::applyMicPolicy() {
    const bool want = m_machine.micShouldBeOpen();
    if (want && !m_audio.running()) {
        resetVad();
        m_listenSinceMs = Clock::monotonicMs();
        m_audio.start(chooseSource(), m_cfg.audio.sampleRate,
                      [this](const float* f, size_t n, int64_t ms) { onAudio(f, n, ms); });
    } else if (!want && m_audio.running()) {
        m_audio.stop();
    }
}

void CDaemon::tick() {
    const int64_t now = Clock::monotonicMs();
    if (m_cfg.compositor.enabled && now - m_lastPollMs >= m_cfg.compositor.pollMs) {
        m_lastPollMs = now;
        m_env        = m_compositor.poll(&m_lastRawStatus);
        m_machine.onEnv(m_env);
    }
    // Safety: a stuck PTT window (client crashed without `ptt stop`) auto-closes.
    if (m_machine.state() == EState::Listening && m_machine.currentActivation() == EActivation::PushToTalk) {
        if (now - m_listenSinceMs > m_cfg.vad.maxUtteranceMs + 5000) {
            Log::log(Log::WARN, "PTT window timed out; closing");
            drainAudio();
            std::vector<SSpeechSegment> segs;
            if (m_vad && m_vad->flush(segs))
                for (auto& s : segs) handleSegment(s);
            m_machine.onPttStop();
            m_machine.onTranscribeDone();
        }
    }
    applyMicPolicy();

    if (m_machine.state() != m_lastState) {
        Log::log(Log::DEBUG, "state {} -> {}", stateName(m_lastState), stateName(m_machine.state()));
        m_lastState = m_machine.state();
    }
}

std::string CDaemon::handleControl(const std::string& cmd) {
    if (cmd == "ptt start" || cmd == "ptt") {
        m_machine.onPttStart();
        applyMicPolicy();
        return "ok: listening";
    }
    if (cmd == "ptt stop") {
        // Flush the in-progress utterance, then settle back to the base gate.
        drainAudio();
        std::vector<SSpeechSegment> segs;
        if (m_vad && m_vad->flush(segs))
            for (auto& s : segs) handleSegment(s);
        m_machine.onPttStop();
        m_machine.onTranscribeDone();
        applyMicPolicy();
        return "ok: stopped";
    }
    if (cmd == "ptt toggle") {
        if (m_machine.state() == EState::Listening)
            return handleControl("ptt stop");
        return handleControl("ptt start");
    }
    if (cmd == "status")
        return statusJson();
    if (cmd == "reload")
        return doReload();
    return "error: unknown command '" + cmd + "' (ptt start|stop|toggle, status, reload)";
}

std::string CDaemon::statusJson() const {
    std::string modeStr = m_cfg.activation.mode == EActivationMode::Auto ? "auto" : (m_cfg.activation.mode == EActivationMode::Ptt ? "ptt" : "wake");
    std::string o = "{";
    o += "\"state\":\"" + std::string(stateName(m_machine.state())) + "\"";
    o += ",\"mode\":\"" + modeStr + "\"";
    o += ",\"micOpen\":" + std::string(m_audio.running() ? "true" : "false");
    o += ",\"compositorAvailable\":" + std::string(m_env.compositorAvailable ? "true" : "false");
    o += ",\"headsetPresent\":" + std::string(m_env.headsetPresent ? "true" : "false");
    o += ",\"atKeyboard\":" + std::string(m_env.atKeyboard ? "true" : "false");
    o += ",\"asrLoaded\":" + std::string(m_asr.loaded() ? "true" : "false");
    o += ",\"asrModel\":\"" + m_cfg.asr.model + "\"";
    o += ",\"dtwWordTimestamps\":" + std::string(m_asr.dtwActive() ? "true" : "false");
    o += ",\"wakePhrase\":\"" + m_cfg.wake.phrase + "\"";
    o += ",\"lastOnsetMs\":" + std::to_string(m_lastOnsetMs);
    o += ",\"lastText\":\"";
    for (char c : m_lastText) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    o += "\"";
    o += ",\"intentEnabled\":" + std::string(m_cfg.intent.enabled ? "true" : "false");
    o += ",\"intentBackend\":\"" + m_cfg.intent.backend + "\"";
    o += ",\"executorDryRun\":" + std::string(m_cfg.executor.dryRun ? "true" : "false");
    o += ",\"lastAction\":\"";
    for (char c : m_lastAction) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    o += "\"";
    o += "}";
    return o;
}

std::string CDaemon::doReload() {
    SConfig                  fresh;
    std::vector<std::string> errors, warnings;
    if (!loadConfigFile(m_configPath, fresh, errors, warnings))
        return "error: reload failed: " + (errors.empty() ? "parse error" : errors.front());

    const std::string oldModel    = m_cfg.asr.model;
    const std::string oldLang     = m_cfg.asr.language;
    const bool        oldTranslate = m_cfg.asr.translate;
    m_cfg                         = fresh;
    for (auto& w : warnings)
        Log::log(Log::WARN, "config: {}", w);

    m_machine.configure(m_cfg.activation.mode, m_cfg.activation.fallback);
    resetVad();

    std::string note = "ok: reloaded";
    if (m_cfg.asr.model != oldModel || m_cfg.asr.language != oldLang || m_cfg.asr.translate != oldTranslate) {
        std::string    asrErr;
        CAsr::SParams  ap{m_cfg.asr.model, m_cfg.asr.language, m_cfg.asr.threads, m_cfg.asr.translate};
        if (!m_asr.load(ap, asrErr)) // reload-safe; frees old ctx, keeps running on failure
            note = "ok: reloaded (ASR model load failed: " + asrErr + ")";
    }
    applyMicPolicy();
    return note;
}

int CDaemon::run(std::atomic<bool>& stop) {
    epoll_event events[8];
    while (!stop.load()) {
        int n = epoll_wait(m_epollFd, events, 8, 500);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            Log::log(Log::ERR, "epoll_wait: {}", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == m_control.fd()) {
                m_control.serviceOnce();
            } else if (fd == m_eventFd) {
                uint64_t v;
                while (read(m_eventFd, &v, sizeof(v)) > 0) {}
                drainAudio();
            } else if (fd == m_timerFd) {
                uint64_t v;
                while (read(m_timerFd, &v, sizeof(v)) > 0) {}
                tick();
            }
        }
    }
    m_audio.stop();
    Log::log(Log::INFO, "hypxrvoiced shutting down");
    return 0;
}
