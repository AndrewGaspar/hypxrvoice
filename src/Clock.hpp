#pragma once

#include <cstdint>
#include <ctime>

// Monotonic time helpers. The spatial-anchoring contract (see README / research
// VOICE-CONTROL.md §4) requires every utterance to carry a speech-onset timestamp
// in CLOCK_MONOTONIC milliseconds, so the compositor's `hyprctl openxr gaze at <ms>`
// can resolve the head pose at the moment speech began. All onset/word timestamps
// in the transcript pipeline are produced here.
namespace Clock {
    // Milliseconds on CLOCK_MONOTONIC (matches the compositor's Time::steadyNow()
    // domain, which is also CLOCK_MONOTONIC).
    inline int64_t monotonicMs() {
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
    }
}
