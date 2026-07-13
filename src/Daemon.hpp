#pragma once

#include "ActivationMachine.hpp"
#include "Asr.hpp"
#include "AudioCapture.hpp"
#include "Compositor.hpp"
#include "Config.hpp"
#include "ControlSocket.hpp"
#include "Vad.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The long-lived hypxrvoiced process. Single-threaded state model: everything
// (state machine, VAD, ASR, control handling, compositor poll) runs on the main
// epoll thread. Only PipeWire's RT callback runs elsewhere, and it merely copies
// samples into a queue and wakes the loop via an eventfd. This keeps the activation
// state authoritative in one place — no cross-thread state races.
class CDaemon {
  public:
    CDaemon() = default;
    ~CDaemon();

    bool init(const std::string& configPath, std::string& err);
    int  run(std::atomic<bool>& stop); // blocks until stop

  private:
    // ---- audio path ----
    struct SChunk {
        std::vector<float> samples;
        int64_t            monoMs = 0;
    };
    void onAudio(const float* frames, size_t n, int64_t monoMs); // PW thread
    void drainAudio();                                           // main thread
    void handleSegment(const SSpeechSegment& seg);

    // ---- mic gating ----
    void applyMicPolicy();      // open/close capture to match the state machine
    std::string chooseSource() const;

    // ---- periodic ----
    void tick(); // ~4 Hz: compositor poll cadence + listen timeout

    // ---- control ----
    std::string handleControl(const std::string& cmd);
    std::string statusJson() const;
    std::string doReload();

    void resetVad();

    SConfig            m_cfg;
    std::string        m_configPath;
    CActivationMachine m_machine;
    CAudioCapture      m_audio;
    CAsr               m_asr;
    CCompositor        m_compositor;
    CControlServer     m_control;

    std::unique_ptr<CVad> m_vad;

    std::mutex         m_qMu;
    std::deque<SChunk> m_queue;

    int m_eventFd = -1;
    int m_timerFd = -1;
    int m_epollFd = -1;

    SEnvSignal m_env{};
    int64_t    m_lastPollMs   = 0;
    int64_t    m_listenSinceMs = 0;
    EState     m_lastState    = EState::GatedKeyboard;

    // last transcript summary for `status`
    std::string m_lastText;
    int64_t     m_lastOnsetMs = 0;
    std::string m_lastRawStatus;
};
