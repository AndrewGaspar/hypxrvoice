#pragma once

#include <string>
#include <vector>

// Load an audio file (WAV/FLAC/etc via libsndfile) as 16 kHz mono float samples,
// resampling and downmixing as needed. Used by the --oneshot debug/test path so the
// whole VAD->ASR->transcript pipeline can run without a microphone.
bool loadAudioMono16k(const std::string& path, std::vector<float>& out, std::string& err);
