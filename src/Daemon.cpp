#include "Daemon.hpp"
#include "Clock.hpp"
#include "DesktopContext.hpp"
#include "Executor.hpp"
#include "Feedback.hpp"
#include "GazeResolver.hpp"
#include "IntentPipeline.hpp"
#include "Log.hpp"
#include "Pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

CDaemon::~CDaemon() {
    m_audio.stop();
    Feedback::stopRuntime();
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
    vc.adaptive        = m_cfg.vad.adaptive;
    vc.noiseFloorFactor = m_cfg.vad.noiseFloorFactor;
    vc.noiseWindowMs   = m_cfg.vad.noiseWindowMs;
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

    loadIntentBackend();

    // WP-H8 feedback runtime: in-headset HUD as a D-Bus client of the shared hypxrhud
    // daemon + terse TTS. Daemon-only — degrades to notify-send when hypxrhud is absent.
    Feedback::startRuntime(m_cfg);

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

void CDaemon::loadIntentBackend() {
    m_llama.reset();
    if (!m_cfg.intent.enabled || m_cfg.intent.backend != "llama")
        return;
    if (m_cfg.intent.model.empty()) {
        Log::log(Log::WARN, "intent.backend=llama but intent.model is empty — using rule backend");
        return;
    }
    auto        llama = std::make_unique<CLlamaIntent>();
    SLlamaParams lp{m_cfg.intent.model, m_cfg.intent.temperature, m_cfg.intent.nThreads,
                    m_cfg.intent.contextMaxMonitors > 0 ? 4096 : 4096, 256};
    std::string err;
    if (llama->load(lp, err)) {
        m_llama = std::move(llama);
    } else {
        Log::log(Log::ERR, "llama intent backend unavailable: {} — using rule backend", err);
    }
}

void CDaemon::onAudio(const float* frames, size_t n, int64_t monoMs) {
    // Live observability (updated on the RT callback thread; lock-free atomics). This
    // is what turns "silent source vs dead path" into a 5-second `status` diagnosis:
    // frames-received proves the PW callback delivers PCM at all, and the rolling
    // RMS/peak show the actual signal level our capture path sees.
    if (n > 0) {
        double acc  = 0;
        float  peak = 0;
        for (size_t i = 0; i < n; i++) {
            const float a = frames[i] < 0 ? -frames[i] : frames[i];
            acc += static_cast<double>(frames[i]) * frames[i];
            if (a > peak)
                peak = a;
        }
        const float rms = static_cast<float>(std::sqrt(acc / n));
        // ~1 s rolling EMA over capture callbacks (they arrive at a few hundred Hz).
        const float prev = m_inRms1e4.load(std::memory_order_relaxed) / 1e4f;
        const float ema  = prev + 0.15f * (rms - prev);
        m_inRms1e4.store(static_cast<uint32_t>(ema * 1e4f), std::memory_order_relaxed);
        // Peak: decay the running peak, but latch any louder frame immediately.
        const float pprev  = m_inPeak1e4.load(std::memory_order_relaxed) / 1e4f;
        const float pdecay = pprev * 0.9f;
        m_inPeak1e4.store(static_cast<uint32_t>(std::max(peak, pdecay) * 1e4f), std::memory_order_relaxed);
        m_framesReceived.fetch_add(n, std::memory_order_relaxed);
    }
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
    updateListeningHud(); // reflect the latest VAD in-speech state on the HUD.
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
    // A real transcript/action panel now owns the HUD; relinquish the listening panel
    // so updateListeningHud() won't hide the action out from under it.
    m_hudListening = false;
    Feedback::emitTranscript(t, m_cfg);

    // WP-V4 intent tier: transcript -> context snapshot -> command -> executor. Uses
    // the live compositor for the read-only snapshot + gaze ring, and the executor's
    // (default dry-run) actuator. All injected here so the pipeline stays testable.
    if (m_cfg.intent.enabled) {
        IntentFn backend;
        if (m_llama && m_llama->loaded()) {
            SIntentConfig ic = IntentPipeline::intentConfig(m_cfg);
            backend = [this, ic](const STranscript& tt, const SDesktopContext& c,
                                 const GazeQueryFn& g) { return m_llama->resolve(tt, c, g, ic); };
        }
        auto r = IntentPipeline::process(t, m_cfg, defaultHyprctlQuery, defaultGazeQuery,
                                         defaultRunner, backend);
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
        m_framesReceived.store(0, std::memory_order_relaxed);
        m_inRms1e4.store(0, std::memory_order_relaxed);
        m_inPeak1e4.store(0, std::memory_order_relaxed);
        m_audio.start(chooseSource(), m_cfg.audio.sampleRate,
                      [this](const float* f, size_t n, int64_t ms) { onAudio(f, n, ms); });
        // NOTE: opening the mic does NOT mean "listening" — in ARMED_WAKEWORD the mic
        // is open at DON but nothing is being transcribed. The listening panel is driven
        // by updateListeningHud() from the actual utterance-capture signal, so the HUD
        // stays honest (no "listening…" merely because the wake word is armed).
    } else if (!want && m_audio.running()) {
        m_audio.stop();
        updateListeningHud(); // capture closed → drop any listening panel we own.
    }
}

void CDaemon::updateListeningHud() {
    // Honest mapping: show "listening…" only while we are ACTUALLY capturing an
    // utterance — VAD in-speech (wake or PTT), or a freshly opened PTT window before
    // onset. ARMED_WAKEWORD alone shows nothing (see applyMicPolicy).
    const bool pttOpen = m_machine.state() == EState::Listening &&
                         m_machine.currentActivation() == EActivation::PushToTalk;
    const bool capturing = (m_vad && m_vad->inSpeech()) || pttOpen;
    if (capturing && !m_hudListening) {
        Feedback::onListeningStart(m_cfg);
        m_hudListening = true;
    } else if (!capturing && m_hudListening) {
        Feedback::onListeningStop(m_cfg);
        m_hudListening = false;
    }
}

void CDaemon::tick() {
    const int64_t now = Clock::monotonicMs();
    if (m_cfg.compositor.enabled && now - m_lastPollMs >= m_cfg.compositor.pollMs) {
        m_lastPollMs = now;
        m_env        = m_compositor.poll(&m_lastRawStatus);
        m_machine.onEnv(m_env);
    }
    // Safety: a stuck PTT window (client crashed without `ptt stop`) auto-closes at its
    // deadline. The deadline is armed when the window OPENS (closePttWindow / onPttStart)
    // and cleared when it closes, so it can never fire on a stale timestamp or stack
    // across repeated toggles.
    if (m_pttDeadlineMs != 0 && m_machine.state() == EState::Listening &&
        m_machine.currentActivation() == EActivation::PushToTalk) {
        if (now >= m_pttDeadlineMs) {
            Log::log(Log::WARN, "PTT window timed out; closing");
            closePttWindow();
        }
    }
    applyMicPolicy();
    updateListeningHud();
    Feedback::pollRuntime(); // drain the hypxrhud bus fd (runtime/ownership signals).

    // Periodic capture telemetry (every ~30 s while the mic is open): the same fields
    // `status` reports, logged so a silent source or dead path is obvious in the journal.
    if (m_audio.running() && now - m_lastAudioLogMs >= 30000) {
        m_lastAudioLogMs = now;
        Log::log(Log::DEBUG, "capture: frames={} inRms={:.4f} inPeak={:.4f} vad[{}] floor={:.4f} thr={:.4f}",
                 m_framesReceived.load(std::memory_order_relaxed),
                 m_inRms1e4.load(std::memory_order_relaxed) / 1e4f,
                 m_inPeak1e4.load(std::memory_order_relaxed) / 1e4f,
                 (m_vad && m_vad->inSpeech()) ? "speech" : "idle",
                 m_vad ? m_vad->noiseFloor() : 0.f, m_vad ? m_vad->threshold() : 0.f);
    }

    if (m_machine.state() != m_lastState) {
        Log::log(Log::DEBUG, "state {} -> {}", stateName(m_lastState), stateName(m_machine.state()));
        m_lastState = m_machine.state();
    }
}

// Close an open PTT capture window: flush any in-progress utterance, settle the state
// machine back to its base gate, and disarm the safety deadline. Single path so the
// deadline is always cleared (no stacking) whether the close came from `ptt stop`,
// `ptt toggle`, or the timeout.
void CDaemon::closePttWindow() {
    drainAudio();
    std::vector<SSpeechSegment> segs;
    if (m_vad && m_vad->flush(segs))
        for (auto& s : segs) handleSegment(s);
    m_machine.onPttStop();
    m_machine.onTranscribeDone();
    m_pttDeadlineMs = 0;
    updateListeningHud();
}

std::string CDaemon::handleControl(const std::string& cmd) {
    if (cmd == "ptt start" || cmd == "ptt") {
        m_machine.onPttStart();
        // Arm the auto-close deadline only if we actually opened a PTT window. Set from
        // "now" on every open (the mic may already have been open in ARMED_WAKEWORD, in
        // which case applyMicPolicy would not have refreshed a start marker) so the
        // window gets its full duration and never times out on a stale timestamp.
        if (m_machine.state() == EState::Listening &&
            m_machine.currentActivation() == EActivation::PushToTalk)
            m_pttDeadlineMs = Clock::monotonicMs() + m_cfg.vad.maxUtteranceMs + 5000;
        applyMicPolicy();
        updateListeningHud();
        return "ok: listening";
    }
    if (cmd == "ptt stop") {
        closePttWindow(); // flush + settle + disarm the deadline (no stacking).
        applyMicPolicy();
        return "ok: stopped";
    }
    if (cmd == "ptt toggle") {
        // True toggle: a second press while a PTT window is open CLOSES it. Only a
        // PTT-initiated window is toggled off — a wake-word capture in progress is left
        // to VAD, not cut short by the PTT key.
        if (m_machine.state() == EState::Listening &&
            m_machine.currentActivation() == EActivation::PushToTalk)
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
    // Live audio observability: input level (rolling ~1 s), VAD state, and the
    // frames-received counter — enough to tell a silent source from a dead path.
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      ",\"inputRms\":%.4f,\"inputPeak\":%.4f,\"framesReceived\":%llu"
                      ",\"vadInSpeech\":%s,\"vadNoiseFloor\":%.4f,\"vadThreshold\":%.4f",
                      m_inRms1e4.load(std::memory_order_relaxed) / 1e4f,
                      m_inPeak1e4.load(std::memory_order_relaxed) / 1e4f,
                      static_cast<unsigned long long>(m_framesReceived.load(std::memory_order_relaxed)),
                      (m_vad && m_vad->inSpeech()) ? "true" : "false",
                      m_vad ? m_vad->noiseFloor() : 0.f, m_vad ? m_vad->threshold() : 0.f);
        o += buf;
    }
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
    const std::string oldBackend  = m_cfg.intent.backend;
    const std::string oldIntentModel = m_cfg.intent.model;
    const bool        oldIntentEnabled = m_cfg.intent.enabled;
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
    if (m_cfg.intent.backend != oldBackend || m_cfg.intent.model != oldIntentModel ||
        m_cfg.intent.enabled != oldIntentEnabled)
        loadIntentBackend();
    // Re-apply feedback config (TTS mode, HUD enable/slot). HUD geometry/opacity now
    // live in hypxrhud's own config — reload hypxrhud for those. See README.
    Feedback::startRuntime(m_cfg);
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
