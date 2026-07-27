#pragma once

#include <string>
#include <vector>

// Load an audio file (WAV/FLAC/etc via libsndfile) as 16 kHz mono float samples,
// resampling and downmixing as needed. Used by the --oneshot debug/test path so the
// whole VAD->ASR->transcript pipeline can run without a microphone.
bool loadAudioMono16k(const std::string& path, std::vector<float>& out, std::string& err);

// Write mono float samples as a 16-bit PCM WAV at `sampleRate`. Used ONLY by the opt-in
// capture-forensics dumps (AudioDump.hpp) — nothing on the normal path writes audio.
// Overwrites `path`. Returns false (with `err`) when the file could not be written.
bool writeWavMono16(const std::string& path, const std::vector<float>& samples,
                    int sampleRate, std::string& err);
