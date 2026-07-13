#pragma once

#include "ActivationMachine.hpp"

#include <string>

// Polls the HypXRland compositor for the presence + at-keyboard signal that drives
// auto-activation. Reads `hyprctl openxr status -j` (the daemon inherits
// HYPRLAND_INSTANCE_SIGNATURE), parses the `userPresence`, `visible`, and
// `handInput.state` fields, and degrades gracefully: if the compositor, the openxr
// section, or the fields are absent, `compositorAvailable` comes back false and the
// activation machine applies its configured fallback (default: wake-armed).
//
// Field semantics (from src/openxr/XRIpc.cpp / OpenXRManager.cpp):
//   userPresence: "yes" | "no" | "unknown" | "unsupported"
//   visible:      "yes" | "no"
//   handInput.state: "active" (hands live => AWAY from keyboard),
//                    "gated (keyboard)" (typing => AT keyboard),
//                    "gated (manual)" | "off" (uninformative => treat as at-keyboard)
class CCompositor {
  public:
    // Runs the query and fills `out`. Always returns; on any failure `out` has
    // compositorAvailable=false. `rawJson` (optional) receives the raw output for
    // debugging / `status` reporting.
    SEnvSignal poll(std::string* rawJson = nullptr);

    // Parse a JSON status document (exposed for unit testing without a compositor).
    static SEnvSignal parseStatus(const std::string& json);

  private:
    bool m_warnedUnavailable = false;
};
