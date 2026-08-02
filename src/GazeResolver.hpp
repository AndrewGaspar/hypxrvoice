#pragma once

#include "Command.hpp"
#include "DesktopContext.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// WP-V4 deixis resolver. A deictic reference ("this", "here") is resolved at the
// timestamp of the WORD that carried it, not at command-execution time — the head
// has usually moved on 1-3 s later (see docs/openxr/05-configuration.md §gaze). We
// query the compositor's read-only gaze ring `hyprctl -j openxr gaze at <ms>`.
//
// Two robustness measures, both config-tunable:
//   - LEAD: gaze leads speech by ~200-600 ms, and ASR word timestamps are ±100-200 ms.
//           We shift the query target earlier by `leadMs` to land on where the eyes
//           were when the word was being planned.
//   - STABILITY WINDOW: we sample several ring points across a small window and take
//           the modal gaze candidate, so a single saccade mid-word can't hijack the
//           reference. The agreement count feeds the action's confidence.

// One parsed `gaze` reply.
struct SGazeSample {
    bool        ok        = false;
    bool        viewValid = false;
    int         monitorId = -1;
    std::string name;
    bool        selected  = false;
    double      dwellSec  = 0.0;
    double      pos[3]    = {0, 0, 0};
    double      quat[4]   = {0, 0, 0, 1};
    double      forward[3]= {0, 0, 0};
    // OPTIONAL gaze-ray/quad intersection, read from `gaze.hitPoint` (or `gaze.point`).
    // Round 8: the compositor SHIPS this now — Hyprland 68a6eb20 ("report the gaze
    // ray/quad hit point in `hyprctl openxr gaze`") closed GAP 4, and openxrGaze()
    // emits `gaze.hitPoint` + `gaze.hitDistM` whenever the ray actually crosses a quad
    // (Hyprland src/openxr/XRIpc.cpp). It stays false when the gaze misses every quad —
    // which is the common case for "here" — and the resolver then projects along
    // `forward`.
    bool        hasHit    = false;
    double      hit[3]    = {0, 0, 0};
    int64_t     timestampMs = 0;
    int64_t     matchedMs   = 0;
    int64_t     requestedMs = 0;
    int64_t     ageMs       = 0;

    // Parse a single `hyprctl -j openxr gaze [at]` JSON reply. Returns ok=false on
    // malformed input or an ok:false compositor reply.
    static SGazeSample parse(const std::string& json);
};

struct SGazeConfig {
    int windowMs = 300; // span of the stability window ending at the lead-shifted target.
    int leadMs   = 200; // shift the query target this far before the word (gaze leads speech).
    int samples  = 5;   // ring points sampled across the window (>=1).
    // PLACEMENT PROJECTION. "Here" is a point in FRONT of the user, never the user's own
    // head. The gaze reply gives an origin + a direction; the point the word designates
    // is origin + direction * placeDistanceM (unless the compositor hands us a real
    // ray/quad intersection). placeMinDistanceM is a hard floor applied to EVERY
    // candidate point — placing a monitor inside the wearer's head is structurally
    // impossible, not merely unlikely.
    double placeDistanceM    = 1.3;
    double placeMinDistanceM = 0.5;
};

// Project the point a place-deixis designates. PURE, and the single definition of what
// "here" means:
//   * `hit` (a real gaze-ray/quad intersection) wins when `hasHit`;
//   * otherwise pos + normalize(forward) * distanceM;
//   * either way the result is pushed out along the ray until it is at least
//     minDistanceM from `pos`.
// A degenerate/absent forward vector falls back to LOCAL_FLOOR forward (0,0,-1), which
// still lands the point outside the head rather than in it. `outDistM` receives the
// final |out - pos|.
void projectPlacePoint(const double pos[3], const double forward[3], bool hasHit,
                       const double hit[3], double distanceM, double minDistanceM,
                       double out[3], double* outDistM);

// Queries `gaze at <ms>` for a given millisecond and returns the raw JSON. Injectable
// so tests drive canned ring data with no compositor. argv is our own constants.
using GazeQueryFn = std::function<std::string(int64_t atMs)>;
std::string defaultGazeQuery(int64_t atMs);

// Resolve a deictic at word time `wordMs` into a stable gaze pick. `ctx` (optional,
// may be null) lets us confirm the picked monitor name is a live, enumerated target.
SGazeResolution resolveDeixis(int64_t wordMs, const SGazeConfig& cfg,
                              const GazeQueryFn& query, const SDesktopContext* ctx);
