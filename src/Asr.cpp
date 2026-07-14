#include "Asr.hpp"
#include "Clock.hpp"
#include "Log.hpp"

#include <whisper.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace {
    // Pick a DTW alignment-heads preset by matching the model file name. Returning
    // WHISPER_AHEADS_NONE means "no DTW for this model" (heuristic fallback).
    whisper_alignment_heads_preset aheadsForModel(const std::string& path) {
        std::string f = std::filesystem::path(path).filename().string();
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return std::tolower(c); });
        auto has = [&](const char* s) { return f.find(s) != std::string::npos; };
        // Order matters: check the more specific ".en" and version variants first.
        if (has("tiny.en")) return WHISPER_AHEADS_TINY_EN;
        if (has("tiny")) return WHISPER_AHEADS_TINY;
        if (has("base.en")) return WHISPER_AHEADS_BASE_EN;
        if (has("base")) return WHISPER_AHEADS_BASE;
        if (has("small.en")) return WHISPER_AHEADS_SMALL_EN;
        if (has("small")) return WHISPER_AHEADS_SMALL;
        if (has("medium.en")) return WHISPER_AHEADS_MEDIUM_EN;
        if (has("medium")) return WHISPER_AHEADS_MEDIUM;
        if (has("large-v3-turbo") || has("large-v3_turbo")) return WHISPER_AHEADS_LARGE_V3_TURBO;
        if (has("large-v3")) return WHISPER_AHEADS_LARGE_V3;
        if (has("large-v2")) return WHISPER_AHEADS_LARGE_V2;
        if (has("large-v1")) return WHISPER_AHEADS_LARGE_V1;
        return WHISPER_AHEADS_NONE;
    }
}

CAsr::~CAsr() {
    if (m_ctx)
        whisper_free(static_cast<whisper_context*>(m_ctx));
    m_ctx = nullptr;
}

bool CAsr::load(const SParams& params, std::string& err) {
    // Reload-safe: free any previously loaded context first.
    if (m_ctx) {
        whisper_free(static_cast<whisper_context*>(m_ctx));
        m_ctx = nullptr;
    }
    m_dtw    = false;
    m_params = params;
    if (params.modelPath.empty()) {
        err = "no ASR model configured (set asr.model in config.toml; run scripts/fetch-models.sh)";
        return false;
    }
    if (!std::filesystem::exists(params.modelPath)) {
        err = "ASR model not found at '" + params.modelPath + "' (run scripts/fetch-models.sh)";
        return false;
    }

    whisper_context_params cparams = whisper_context_default_params();
    // whisper_context_default_params() defaults flash_attn=true, and whisper SILENTLY
    // disables DTW token timestamps when flash_attn is on ("dtw_token_timestamps is not
    // supported with flash_attn - disabling"). DTW word timestamps are REQUIRED (the
    // gaze word-time contract), so turn flash_attn OFF — this box runs whisper on CPU
    // anyway, where flash_attn buys nothing. m_dtw below now reflects whisper's REAL
    // state (DTW genuinely engages) rather than merely what we requested.
    cparams.flash_attn = false;
    whisper_alignment_heads_preset preset = aheadsForModel(params.modelPath);
    if (preset != WHISPER_AHEADS_NONE) {
        cparams.dtw_token_timestamps = true;
        cparams.dtw_aheads_preset    = preset;
        m_dtw                        = true;
    } else {
        Log::log(Log::WARN, "no DTW alignment-heads preset for model '{}'; using heuristic word timestamps", params.modelPath);
        m_dtw = false;
    }

    whisper_context* ctx = whisper_init_from_file_with_params(params.modelPath.c_str(), cparams);
    if (!ctx) {
        // DTW init can fail on quantized/odd models; retry once without DTW.
        if (m_dtw) {
            Log::log(Log::WARN, "DTW-enabled model load failed; retrying without DTW");
            m_dtw                        = false;
            cparams.dtw_token_timestamps = false;
            ctx                          = whisper_init_from_file_with_params(params.modelPath.c_str(), cparams);
        }
        if (!ctx) {
            err = "failed to load ASR model '" + params.modelPath + "'";
            return false;
        }
    }
    m_ctx = ctx;
    Log::log(Log::INFO, "ASR model loaded: {} (DTW word timestamps: {})", params.modelPath, m_dtw ? "on" : "off");
    return true;
}

STranscript CAsr::transcribe(const std::vector<float>& samples, int64_t bufferStartMs, int64_t onsetMs, int64_t endMs, EActivation activation) {
    STranscript out;
    out.onsetMs    = onsetMs;
    out.endMs      = endMs;
    out.wallMs     = std::time(nullptr) * 1000LL;
    out.activation = activation;

    if (!m_ctx || samples.empty())
        return out;

    auto* ctx = static_cast<whisper_context*>(m_ctx);

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.print_progress   = false;
    wp.print_realtime   = false;
    wp.print_timestamps = false;
    wp.print_special    = false;
    wp.translate        = m_params.translate;
    wp.language         = m_params.language.c_str();
    wp.n_threads        = std::max(1, m_params.threads);
    wp.token_timestamps = true; // needed for word boundaries (and DTW fields)
    wp.no_timestamps    = false;
    wp.single_segment   = false;

    if (whisper_full(ctx, wp, samples.data(), static_cast<int>(samples.size())) != 0) {
        Log::log(Log::ERR, "whisper_full failed");
        return out;
    }

    // whisper timestamps are in 10 ms units; convert to absolute monotonic ms.
    auto toAbsMs = [&](int64_t t10ms) { return bufferStartMs + t10ms * 10; };

    const whisper_token eot = whisper_token_eot(ctx);
    const int           nseg = whisper_full_n_segments(ctx);

    SWord              cur;
    bool               haveWord = false;
    float              probAcc  = 0.f;
    int                probN    = 0;
    std::string        fullText;

    auto flushWord = [&]() {
        if (!haveWord)
            return;
        // Trim leading/trailing whitespace on the word text.
        size_t a = cur.text.find_first_not_of(" \t");
        size_t b = cur.text.find_last_not_of(" \t");
        if (a == std::string::npos) {
            haveWord = false;
            cur      = SWord{};
            probAcc  = 0;
            probN    = 0;
            return;
        }
        cur.text = cur.text.substr(a, b - a + 1);
        cur.prob = probN > 0 ? probAcc / probN : -1.f;
        out.words.push_back(cur);
        cur      = SWord{};
        haveWord = false;
        probAcc  = 0;
        probN    = 0;
    };

    for (int s = 0; s < nseg; s++) {
        const int ntok = whisper_full_n_tokens(ctx, s);
        for (int t = 0; t < ntok; t++) {
            whisper_token_data td   = whisper_full_get_token_data(ctx, s, t);
            if (td.id >= eot)
                continue; // special / timestamp token
            const char* txt = whisper_full_get_token_text(ctx, s, t);
            if (!txt || !*txt)
                continue;
            std::string piece = txt;

            // Timestamps for this token: prefer DTW when active and valid.
            int64_t tstart = (m_dtw && td.t_dtw >= 0) ? td.t_dtw : td.t0;
            int64_t tend   = (m_dtw && td.t_dtw >= 0) ? td.t_dtw : td.t1;

            // A leading space marks a new word boundary.
            const bool boundary = !piece.empty() && (piece[0] == ' ');
            if (boundary)
                flushWord();

            if (!haveWord) {
                cur.startMs = toAbsMs(tstart);
                cur.endMs   = toAbsMs(tend);
                haveWord    = true;
            }
            cur.text += piece;
            cur.endMs = std::max(cur.endMs, toAbsMs(tend));
            probAcc += td.p;
            probN++;
            fullText += piece;
        }
    }
    flushWord();

    // Normalize the full text: trim.
    size_t a = fullText.find_first_not_of(" \t\n");
    size_t b = fullText.find_last_not_of(" \t\n");
    out.text = (a == std::string::npos) ? "" : fullText.substr(a, b - a + 1);

    // Enforce monotonic, non-decreasing word timestamps (DTW can occasionally emit
    // a tiny inversion; downstream gaze resolution assumes ordering).
    for (size_t i = 1; i < out.words.size(); i++) {
        if (out.words[i].startMs < out.words[i - 1].startMs)
            out.words[i].startMs = out.words[i - 1].startMs;
        if (out.words[i].endMs < out.words[i].startMs)
            out.words[i].endMs = out.words[i].startMs;
    }

    return out;
}
