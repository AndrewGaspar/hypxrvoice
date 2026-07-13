#include "Wav.hpp"

#include <sndfile.h>

#include <cmath>
#include <vector>

bool loadAudioMono16k(const std::string& path, std::vector<float>& out, std::string& err) {
    SF_INFO  info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) {
        err = std::string("sf_open('") + path + "') failed: " + sf_strerror(nullptr);
        return false;
    }

    const int channels = info.channels > 0 ? info.channels : 1;
    std::vector<float> interleaved(static_cast<size_t>(info.frames) * channels);
    sf_count_t read = sf_readf_float(f, interleaved.data(), info.frames);
    sf_close(f);
    if (read <= 0) {
        err = "no audio frames read from '" + path + "'";
        return false;
    }

    // Downmix to mono.
    std::vector<float> mono(static_cast<size_t>(read));
    for (sf_count_t i = 0; i < read; i++) {
        float acc = 0;
        for (int c = 0; c < channels; c++)
            acc += interleaved[i * channels + c];
        mono[i] = acc / channels;
    }

    const int srcRate = info.samplerate;
    const int dstRate = 16000;
    if (srcRate == dstRate) {
        out = std::move(mono);
        return true;
    }

    // Linear resample to 16 kHz. Adequate for ASR (whisper is robust to it); a
    // polyphase resampler would be marginally better but adds a dependency.
    const double ratio  = static_cast<double>(dstRate) / srcRate;
    const size_t outLen = static_cast<size_t>(std::llround(mono.size() * ratio));
    out.resize(outLen);
    for (size_t i = 0; i < outLen; i++) {
        double srcPos = i / ratio;
        size_t i0     = static_cast<size_t>(srcPos);
        size_t i1     = std::min(i0 + 1, mono.size() - 1);
        double frac   = srcPos - i0;
        out[i]        = static_cast<float>(mono[i0] * (1.0 - frac) + mono[i1] * frac);
    }
    return true;
}
