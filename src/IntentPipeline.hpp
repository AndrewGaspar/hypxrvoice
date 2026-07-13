#pragma once

#include "Command.hpp"
#include "Config.hpp"
#include "DesktopContext.hpp"
#include "Executor.hpp"
#include "GazeResolver.hpp"
#include "Intent.hpp"
#include "Transcript.hpp"

#include <functional>

// WP-V4 wiring: transcript -> context snapshot -> intent -> feedback sink -> executor.
// Every external dependency (compositor queries, gaze ring, the process runner, and
// the intent backend) is injectable so the whole chain runs offline in unit tests
// with fixture JSON and a mocked hyprctl that merely records argv.

// A pluggable intent backend. Default is the rule-based one; the llama backend
// (LlamaIntent.hpp) supplies the same signature when HAVE_LLAMA and a model is set.
using IntentFn = std::function<SAction(const STranscript&, const SDesktopContext&, const GazeQueryFn&)>;

namespace IntentPipeline {
    SIntentConfig intentConfig(const SConfig& cfg);
    SExecConfig   execConfig(const SConfig& cfg);

    struct SResult {
        SDesktopContext ctx;
        SAction         action;
        SExecPlan       plan;
        int             dispatched = 0; // steps actually run (0 in dry-run).
    };

    // Run one transcript through the full chain. `contextQuery` fetches the three
    // hyprctl snapshots; `gazeQuery` serves the gaze ring; `runner` executes plan
    // steps (or is ignored in dry-run); `backend` overrides the default rule engine.
    SResult process(const STranscript& t, const SConfig& cfg,
                    const QueryFn& contextQuery, const GazeQueryFn& gazeQuery,
                    const RunFn& runner, const IntentFn& backend = nullptr);
}
