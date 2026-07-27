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
    const SVadConfig vc = Pipeline::vadConfig(m_cfg);
    m_vad               = std::make_unique<CVad>(vc);
    // The PTT buffer scales its presence verdict and its length cap off the same numbers.
    m_pttAudio.configure(vc);
    m_dump.noteVadConfig(vc);
}

// A PUSH-TO-TALK window is open: the machine is Listening and a press (not the wake
// word) is what opened it. Asked in enough places that it earns a name.
bool CDaemon::pttListening() const {
    return m_machine.state() == EState::Listening &&
           m_machine.currentActivation() == EActivation::PushToTalk;
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
    applyDumpConfig();
    resetVad();
    m_preRoll.configure(m_cfg.audio.sampleRate, m_cfg.capture.preRollMs);

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

// Apply debug.dump_audio_dir. Off by default; while it is ON the daemon says so on
// every (re)load, because it means microphone audio is landing on disk.
void CDaemon::applyDumpConfig() {
    std::string err;
    if (!m_dump.configure(m_cfg.debug.dumpAudioDir, m_cfg.debug.dumpAudioKeep,
                          m_cfg.audio.sampleRate, err))
        Log::log(Log::ERR, "audio dump disabled: {}", err);
    else if (m_dump.enabled())
        Log::log(Log::WARN, "debug.dump_audio_dir is SET — capture audio is being written to {} "
                            "(last {} windows); unset it when you are done",
                 m_dump.dir(), m_cfg.debug.dumpAudioKeep);
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
    // THE GATE. While it is shut the frames are held ONLY in the pre-roll ring: no VAD,
    // no ASR, no wake-word tier, nothing written anywhere. The stream may be connected
    // (so the next window starts instantly) but this is what makes that harmless.
    if (!m_captureActive) {
        for (auto& c : local)
            m_preRoll.push(c.samples.data(), c.samples.size(), c.monoMs);
        return;
    }
    if (!m_vad)
        return;

    std::vector<SSpeechSegment> segs;
    for (auto& c : local) {
        m_dump.appendWindow(c.samples.data(), c.samples.size(), c.monoMs);
        if (m_pttWholeWindow && m_pttSpent)
            continue; // this window already produced its transcript; audio goes nowhere
        if (m_pttWholeWindow)
            m_pttAudio.push(c.samples.data(), c.samples.size(), c.monoMs);
        m_vad->push(c.samples.data(), c.samples.size(), c.monoMs, segs);
    }

    if (m_pttWholeWindow) {
        // The VAD no longer decides WHAT is transcribed here — only WHEN. A completed
        // segment means the speaker stopped, so the whole window goes to whisper now
        // rather than waiting out a toggle the user may have forgotten to press.
        if (!segs.empty() && !m_pttSpent) {
            m_pttVadOnsetMs = segs.front().onsetMs;
            finalizePttUtterance("vad endpoint");
        }
    } else {
        for (auto& s : segs)
            handleSegment(s);
    }
    updateListeningHud(); // reflect the latest VAD in-speech state on the HUD.
}

// How long an already-transcribed PTT window lingers before it auto-closes. The shipped
// bind is `ptt toggle`, so the user's second press is what normally closes the window;
// after an early endpoint that press must still land on an OPEN window (otherwise it
// would open a fresh one and the daemon would sit listening at nothing). Lingering keeps
// the toggle honest, and the timer closes the mic promptly when no press arrives.
static constexpr int64_t kPttSpentLingerMs = 2000;

void CDaemon::finalizePttUtterance(const char* why) {
    if (!m_pttWholeWindow || m_pttSpent)
        return;
    m_pttSpent = true;

    SPttUtterance u = m_pttAudio.finish();
    Log::log(Log::DEBUG,
             "ptt window ({}): {}ms captured, speech={} ({}ms over thr {:.4f}; floor {:.4f} peak {:.4f}){}",
             why, u.endMs - u.startMs, u.speech ? "yes" : "no", u.presence.voicedMs,
             u.presence.threshold, u.presence.floorRms, u.presence.peakRms,
             u.truncated ? " [truncated to max_utterance_ms]" : "");

    // Nothing audible anywhere in the window: let closePttWindow's !m_windowProduced path
    // say "didn't hear anything" rather than spending a whisper pass on silence.
    if (!u.speech)
        return;

    SSpeechSegment seg;
    seg.bufferStartMs = u.startMs;
    seg.endMs         = u.endMs;
    // Anchor on the VAD's onset when it fired (a better instant for gaze than the press),
    // else on the window itself. Word timestamps are offset from bufferStartMs regardless.
    seg.onsetMs = (m_pttVadOnsetMs > u.startMs && m_pttVadOnsetMs < u.endMs) ? m_pttVadOnsetMs : u.startMs;
    seg.samples = std::move(u.samples);
    handleSegment(seg);

    // Early endpoint: keep the window open just long enough for the toggle's second press.
    if (m_pttDeadlineMs != 0)
        m_pttDeadlineMs = Clock::monotonicMs() + kPttSpentLingerMs;
}

void CDaemon::handleSegment(const SSpeechSegment& seg) {
    // The backpad is the number that says whether a quiet first syllable could have
    // survived at all (see SVadConfig::onsetBackpadMs); log it on every segment so the
    // journal alone distinguishes "cut short" from "never arrived".
    Log::log(Log::DEBUG, "segment: onset {}ms len {}ms backpad {}ms ({} samples)", seg.onsetMs,
             seg.endMs - seg.onsetMs, seg.backpadMs(), seg.samples.size());
    m_dump.noteSegment(seg);

    const bool pttWindow = pttListening();
    const bool requireWake = !pttWindow;
    const EActivation act = pttWindow ? EActivation::PushToTalk : EActivation::WakeWord;

    if (!m_asr.loaded()) {
        Log::log(Log::WARN, "dropping {}ms segment — no ASR model loaded", seg.endMs - seg.onsetMs);
        if (pttWindow) {
            m_lastText = "";
            noteWindowResult("asr-unavailable");
            Feedback::emitRejected("", m_cfg, "no speech model loaded");
        }
        return;
    }

    STranscript t;
    if (!Pipeline::processSegment(m_asr, m_cfg, seg, act, requireWake, t)) {
        Log::log(Log::DEBUG, "segment rejected (onset {}ms): not addressed / empty", seg.onsetMs);
        // A PTT window is an EXPLICIT request — never let it end in silence. (A wake-word
        // window that simply wasn't addressed to us stays quiet by design: that is the
        // whole point of the wake phrase.)
        if (pttWindow) {
            m_lastText = t.text; // "" when ASR found nothing; verbatim otherwise
            noteWindowResult(t.text.empty() ? "no-speech" : "unparsed");
            Feedback::emitRejected(t.text, m_cfg);
        }
        return;
    }
    m_lastText    = t.text;
    m_lastOnsetMs = t.onsetMs;
    // A real transcript/action panel now owns the HUD; relinquish the listening panel
    // so updateListeningHud() won't hide the action out from under it — or raise a stale
    // "listening…" back over it while the PTT key is still down.
    noteWindowResult("ok");
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
        m_lastAction = std::string(verbName(r.action.verb));
        if (r.action.verb == EVerb::Workspace)
            m_lastAction += " " + std::to_string(r.action.workspace);
        else if (!r.action.windowLabel.empty())
            m_lastAction += " " + r.action.windowLabel;
        else if (!r.action.target.empty())
            m_lastAction += " " + r.action.target;
        // The intent tier owns the HUD from here: EVerb::None means it showed the
        // transcript back as a rejection panel, which `status` should say too.
        if (r.action.verb == EVerb::None)
            m_lastOutcome = "unparsed";
    }
}

std::string CDaemon::chooseSource() const {
    // In the headset, prefer the headset mic (wivrn.source); otherwise the configured
    // desk source (empty => PipeWire default).
    if (m_env.headsetPresent && !m_cfg.audio.headsetSource.empty())
        return m_cfg.audio.headsetSource;
    return m_cfg.audio.source;
}

// Should the PipeWire stream stay CONNECTED even with the gate shut?
//
// Only for the headset mic. `wivrn.source` suspends the moment nothing reads it, and
// resuming it makes the WiVRn server ask the headset client to start its microphone over
// the network — the ~0.5–1 s that swallowed the leading verb of every PTT utterance. A
// desk source opens locally and fast, so nothing is gained by holding it and the stricter
// "no stream unless a window is open" stance is kept there.
bool CDaemon::captureShouldBeHeld() const {
    if (!m_cfg.capture.hold)
        return false;
    return m_env.headsetPresent && !m_cfg.audio.headsetSource.empty();
}

// (Re)connect the capture stream. The caller owns the backoff gate; this function ARMS
// the next one unconditionally — including on success, because "connects then immediately
// errors" (the WiVRn-source-vanished shape) is exactly the case that would otherwise be
// retried at the 4 Hz tick rate forever. A stream that actually delivers PCM clears the
// backoff again from tick().
void CDaemon::startCapture(const std::string& source) {
    const int64_t now = Clock::monotonicMs();

    m_audio.stop(); // no-op when closed; also the teardown for a source change / failure
    m_framesReceived.store(0, std::memory_order_relaxed);
    m_inRms1e4.store(0, std::memory_order_relaxed);
    m_inPeak1e4.store(0, std::memory_order_relaxed);
    m_lastFrameCount   = 0;
    m_lastFrameChangeMs = now;
    m_preRoll.clear(); // pre-restart audio is not contiguous with what follows
    if (m_captureActive)
        resetVad();

    const bool ok      = m_audio.start(source, m_cfg.audio.sampleRate,
                                       [this](const float* f, size_t n, int64_t ms) { onAudio(f, n, ms); });
    m_captureBackoffMs = m_captureBackoffMs > 0 ? std::min(m_captureBackoffMs * 2, 30000) : 1000;
    m_captureRetryAtMs = now + m_captureBackoffMs;
    if (!ok)
        Log::log(Log::WARN, "audio capture unavailable (source '{}'); retrying in {}ms",
                 source.empty() ? "default" : source, m_captureBackoffMs);
}

// The gate just opened. Start a clean VAD and seed it with the pre-roll ring, so an
// utterance that began at (or just before) the PTT press is transcribed in full instead
// of arriving with its first word missing.
void CDaemon::openCaptureWindow() {
    resetVad();
    m_dump.beginWindow(Clock::monotonicMs());

    // A PRESS is an explicit declaration that speech is coming, so this window is
    // transcribed WHOLE and the VAD is demoted to endpointing (see PttWindow.hpp). The
    // wake-word gate, which has no press to trust, keeps the onset-segmented path.
    m_pttWholeWindow = m_cfg.capture.pttWholeWindow && pttListening();
    m_pttSpent       = false;
    m_pttVadOnsetMs  = 0;
    if (m_pttWholeWindow)
        m_pttAudio.begin();

    std::vector<float> pre;
    int64_t            preStartMs = 0;
    const size_t       n          = m_preRoll.drain(pre, preStartMs);
    if (n == 0)
        return;
    m_dump.notePreRoll(pre.data(), pre.size(), preStartMs);
    if (m_pttWholeWindow)
        m_pttAudio.push(pre.data(), pre.size(), preStartMs);
    std::vector<SSpeechSegment> segs;
    m_vad->push(pre.data(), pre.size(), preStartMs, segs);
    // The ring is shorter than an utterance, but a segment CAN close inside it if the
    // user spoke and stopped before the key even registered — keep it rather than drop it.
    // In a whole-window PTT window that is an endpoint signal, not a transcript unit.
    if (m_pttWholeWindow) {
        if (!segs.empty()) {
            m_pttVadOnsetMs = segs.front().onsetMs;
            finalizePttUtterance("vad endpoint in pre-roll");
        }
    } else {
        for (auto& s : segs)
            handleSegment(s);
    }
    Log::log(Log::DEBUG, "capture window opened with {}ms of pre-roll",
             (n * 1000) / static_cast<size_t>(m_cfg.audio.sampleRate));
}

void CDaemon::applyMicPolicy() {
    const bool        active = m_machine.micShouldBeOpen();
    const bool        hold   = captureShouldBeHeld();
    const std::string source = chooseSource();

    // ---- 1. stream lifetime ----
    if (active || hold) {
        const bool failed = m_audio.failed();
        const bool need   = !m_audio.running() || m_audio.source() != source || failed;
        // Re-check the backoff here as well as inside startCapture, so a stream that is
        // down for good doesn't log a reconnect line at the 4 Hz tick rate.
        if (need && Clock::monotonicMs() >= m_captureRetryAtMs) {
            if (failed)
                Log::log(Log::WARN, "capture stream unusable (state {}); reconnecting", m_audio.stateName());
            startCapture(source);
        }
    } else if (m_audio.running()) {
        m_audio.stop();
        m_preRoll.clear();
        m_captureBackoffMs = 0;
        m_captureRetryAtMs = 0;
    }

    // ---- 2. the gate ----
    if (active && !m_captureActive) {
        m_captureActive = true;
        openCaptureWindow();
        // NOTE: an open gate does NOT mean "listening" — in ARMED_WAKEWORD it is open at
        // DON but nothing is being transcribed. The listening panel is driven by
        // updateListeningHud() from the actual utterance-capture signal, so the HUD stays
        // honest (no "listening…" merely because the wake word is armed).
    } else if (!active && m_captureActive) {
        m_captureActive = false;
        // closePttWindow() always finalizes before the machine leaves Listening, so by the
        // time the gate shuts there is nothing left to transcribe; drop the buffer so a
        // window that ended some other way cannot leak audio into the next one.
        if (m_pttWholeWindow) {
            m_pttAudio.finish();
            m_pttWholeWindow = false;
            m_pttSpent       = false;
        }
        m_dump.endWindow(m_lastOutcome, m_lastText);
        m_preRoll.clear(); // start the next window's pre-roll from post-window audio only
        updateListeningHud(); // gate closed → drop any listening panel we own.
    }
}

void CDaemon::noteWindowResult(const char* outcome) {
    m_windowProduced = true;
    m_hudResultShown = true;
    m_hudListening   = false;
    m_lastOutcome    = outcome;
}

void CDaemon::updateListeningHud() {
    // Honest mapping: show "listening…" only while we are ACTUALLY capturing an
    // utterance — VAD in-speech (wake or PTT), or a freshly opened PTT window before
    // onset. ARMED_WAKEWORD alone shows nothing (see applyMicPolicy).
    const bool capturing = (m_vad && m_vad->inSpeech()) || pttListening();

    if (!capturing) {
        if (m_hudListening) {
            Feedback::onListeningStop(m_cfg);
            m_hudListening = false;
        }
        m_hudResultShown = false; // the next capture may raise a fresh listening panel
        return;
    }
    // Capturing — but do NOT raise "listening…" back over a result panel this window has
    // already produced. A PTT window stays "open" until the key is released, and blindly
    // re-raising here is what buried the transcript/action panel under "listening…" for a
    // user who paused before letting go.
    if (!m_hudListening && !m_hudResultShown) {
        Feedback::onListeningStart(m_cfg);
        m_hudListening = true;
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
    if (m_pttDeadlineMs != 0 && pttListening()) {
        if (now >= m_pttDeadlineMs) {
            // Not a warning when the window already said its piece and was merely waiting
            // out the toggle's second press.
            Log::log(m_pttSpent ? Log::DEBUG : Log::WARN, "PTT window {}; closing",
                     m_pttSpent ? "linger elapsed" : "timed out");
            closePttWindow();
        }
    }
    applyMicPolicy();
    updateListeningHud();
    Feedback::pollRuntime(); // drain the hypxrhud bus fd (runtime/ownership signals).

    // Stall watchdog for the HELD stream: a WiVRn disconnect can leave the node in place
    // while PCM simply stops arriving, which the PipeWire stream state never reports. Only
    // acted on while the gate is SHUT — a reconnect there is invisible, whereas doing it
    // mid-window would cut the user off mid-sentence.
    if (m_audio.running()) {
        const uint64_t frames = m_framesReceived.load(std::memory_order_relaxed);
        if (frames != m_lastFrameCount) {
            m_lastFrameCount    = frames;
            m_lastFrameChangeMs = now;
            // PCM is flowing: this connection is healthy, so forget the reconnect backoff.
            m_captureBackoffMs = 0;
            m_captureRetryAtMs = 0;
        } else if (!m_captureActive && m_lastFrameChangeMs != 0 &&
                   now - m_lastFrameChangeMs >= 10000 && now >= m_captureRetryAtMs) {
            Log::log(Log::WARN, "held capture stalled ({}s without frames); reconnecting",
                     (now - m_lastFrameChangeMs) / 1000);
            startCapture(chooseSource());
        }
    }

    // Periodic capture telemetry (every ~30 s while the stream is up): the same fields
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
    // Closing a window that is not open must not manufacture a "didn't hear anything":
    // a stray `ptt stop` (or a second one after a timeout) has nothing to report.
    if (!pttListening()) {
        m_pttDeadlineMs  = 0;
        m_windowProduced = false;
        return;
    }
    drainAudio();
    if (m_pttWholeWindow) {
        // The whole window IS the utterance; the VAD's in-progress segment is irrelevant.
        finalizePttUtterance("window closed");
        m_pttWholeWindow = false;
    } else {
        std::vector<SSpeechSegment> segs;
        if (m_vad && m_vad->flush(segs))
            for (auto& s : segs) handleSegment(s);
    }
    m_machine.onPttStop();
    m_machine.onTranscribeDone();
    m_pttDeadlineMs = 0;
    updateListeningHud();
    // Never end an explicitly requested window in silence. If VAD found nothing at all,
    // say "didn't hear anything" rather than just dropping the "listening…" panel and
    // leaving the user unable to tell a dead mic from a grammar miss (WP-V6).
    if (!m_windowProduced) {
        m_lastText    = "";
        m_lastOutcome = "no-speech";
        Feedback::emitRejected("", m_cfg);
    }
    m_windowProduced = false;
}

std::string CDaemon::handleControl(const std::string& cmd) {
    if (cmd == "ptt start" || cmd == "ptt") {
        m_windowProduced = false; // fresh window: nothing shown yet (see closePttWindow)
        m_machine.onPttStart();
        // Arm the auto-close deadline only if we actually opened a PTT window. Set from
        // "now" on every open (the mic may already have been open in ARMED_WAKEWORD, in
        // which case applyMicPolicy would not have refreshed a start marker) so the
        // window gets its full duration and never times out on a stale timestamp.
        if (pttListening())
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
        if (pttListening())
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
    // micOpen = the PCM stream is CONNECTED. Since WP-V6 that no longer implies anything
    // is being listened to: captureActive is the gate (frames reaching VAD/ASR), and
    // captureHeld means the stream is deliberately parked open with the gate shut so the
    // next window starts instantly. captureState is the raw PipeWire stream state.
    o += ",\"micOpen\":" + std::string(m_audio.running() ? "true" : "false");
    o += ",\"captureActive\":" + std::string(m_captureActive ? "true" : "false");
    o += ",\"captureHeld\":" + std::string(m_audio.running() && !m_captureActive ? "true" : "false");
    o += ",\"captureState\":\"" + std::string(m_audio.stateName()) + "\"";
    o += ",\"preRollMs\":" + std::to_string(m_cfg.capture.preRollMs);
    // Is the OPEN window one that will be transcribed whole (PTT), rather than sliced by
    // VAD onset? Round-4 diagnosis wants this in one place with the rest of the capture state.
    o += ",\"pttWholeWindow\":" + std::string(m_pttWholeWindow ? "true" : "false");
    o += ",\"preRollBufferedMs\":" + std::to_string(m_preRoll.bufferedMs());
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
    // How the last window ENDED — "ok" | "unparsed" | "no-speech" | "asr-unavailable".
    // Without it an empty lastText is ambiguous between "never ran" and "heard nothing".
    o += ",\"lastOutcome\":\"" + m_lastOutcome + "\"";
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
    applyDumpConfig();
    resetVad();
    m_preRoll.configure(m_cfg.audio.sampleRate, m_cfg.capture.preRollMs);

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
