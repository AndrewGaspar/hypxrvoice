#pragma once

#include "Command.hpp"
#include "DesktopContext.hpp"
#include "GazeResolver.hpp"
#include "Intent.hpp"
#include "Transcript.hpp"

#include <memory>
#include <string>

// WP-V4 local-LLM intent backend (subprojects/llama.cpp). A small instruct GGUF is
// handed the desktop-context digest + the transcript and produces a GBNF-constrained
// raw-intent JSON — the grammar makes non-schema output impossible and restricts the
// target to enumerated live monitor names. The parsed raw intent then flows through
// the SHARED finalizeAction (deixis via the gaze ring, target validation), identical
// to the rule backend.
//
// Compiled only when HAVE_LLAMA (CMake found/built subprojects/llama.cpp). When
// absent, the daemon falls back to CRuleIntent.

struct SLlamaParams {
    std::string modelPath;
    double      temperature = 0.0; // 0 = greedy (deterministic).
    int         nThreads    = 4;
    int         nCtx        = 4096;
    int         nPredict    = 256; // hard cap on generated tokens.
};

class CLlamaIntent {
  public:
    CLlamaIntent();
    ~CLlamaIntent();
    CLlamaIntent(const CLlamaIntent&)            = delete;
    CLlamaIntent& operator=(const CLlamaIntent&) = delete;

    // Load the GGUF. Returns false (with `err`) on failure; the caller then falls back
    // to the rule backend.
    bool load(const SLlamaParams& params, std::string& err);
    bool loaded() const;

    // Full path: build grammar+prompt from the live snapshot, generate, parse the raw
    // intent, and run the shared finalize. `gazeQuery` serves deixis. On any model or
    // parse failure, returns an EVerb::None action (never actuates) with a note.
    SAction resolve(const STranscript& t, const SDesktopContext& ctx,
                    const GazeQueryFn& gazeQuery, const SIntentConfig& icfg);

    // Exposed for tests/inspection: the exact prompt this backend would send.
    std::string buildPrompt(const STranscript& t, const SDesktopContext& ctx,
                            const SIntentConfig& icfg) const;

    // Parse the model's raw-intent JSON into a backend-neutral SRawIntent. Static +
    // pure so it is unit-testable without a model. `deicticMs`/`deicticPlace` come from
    // the transcript's located deictic (the model does not know timestamps).
    static SRawIntent parseRaw(const std::string& json, int64_t deicticMs, bool deicticPlace);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    SLlamaParams          m_params;
};
