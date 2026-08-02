#include "IntentPipeline.hpp"
#include "Feedback.hpp"

namespace IntentPipeline {
    SIntentConfig intentConfig(const SConfig& cfg) {
        SIntentConfig ic;
        ic.gaze.windowMs = cfg.intent.deixisWindowMs;
        ic.gaze.leadMs   = cfg.intent.deixisLeadMs;
        ic.gaze.samples  = cfg.intent.deixisSamples;
        ic.gaze.placeDistanceM    = cfg.intent.placeDistanceM;
        ic.gaze.placeMinDistanceM = cfg.intent.placeMinDistanceM;
        ic.distanceStep  = cfg.intent.distanceStep;
        return ic;
    }

    SExecConfig execConfig(const SConfig& cfg) {
        SExecConfig ec;
        ec.dryRun         = cfg.executor.dryRun;
        ec.allowXrmonitor = cfg.executor.allowXrmonitor;
        ec.allowLaunch    = cfg.executor.allowLaunch;
        ec.allowWindow    = cfg.executor.allowWindow;
        ec.allowCreateMonitor = cfg.executor.allowCreateMonitor;
        ec.distanceStep   = cfg.intent.distanceStep;
        ec.appAllowlist   = cfg.apps;
        ec.caps.targetedGrab = cfg.executor.targetedGrab;
        ec.caps.placeAtPose  = cfg.executor.placeAtPose;
        return ec;
    }

    SResult process(const STranscript& t, const SConfig& cfg,
                    const QueryFn& contextQuery, const GazeQueryFn& gazeQuery,
                    const RunFn& runner, const IntentFn& backend) {
        SResult r;
        // 1. Desktop-context snapshot at utterance time.
        r.ctx = snapshotDesktop(contextQuery);

        // 2. Intent: backend override, else the deterministic rule engine.
        if (backend) {
            r.action = backend(t, r.ctx, gazeQuery);
        } else {
            CRuleIntent rule(intentConfig(cfg));
            r.action = rule.resolve(t, r.ctx, gazeQuery);
        }

        // 3. Plan (pure) + 4. feedback sink + 5. execute (or dry-run).
        SExecConfig ec = execConfig(cfg);
        r.plan         = planFor(r.action, r.ctx, ec);
        // The transcript rides along so an unparseable utterance can be shown back to the
        // user instead of silently hiding the HUD (WP-V6 silent-rejection fix).
        Feedback::emitAction(r.action, r.plan, cfg, t.text);
        r.dispatched = runPlan(r.plan, ec, runner);
        return r;
    }
}
