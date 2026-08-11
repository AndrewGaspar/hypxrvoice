#include "LlamaIntent.hpp"
#include "Grammar.hpp"
#include "Log.hpp"

#include <jansson.h>

#include <cctype>

// ----------------------------------------------------------------------------
// Pure, model-independent parts (compiled regardless of HAVE_LLAMA).
// ----------------------------------------------------------------------------
namespace {
    std::string jstr(json_t* o, const char* k) {
        json_t* v = json_object_get(o, k);
        return (v && json_is_string(v)) ? json_string_value(v) : "";
    }
    bool jbool(json_t* o, const char* k) {
        json_t* v = json_object_get(o, k);
        return v && json_is_true(v);
    }
    double jnum(json_t* o, const char* k, double def) {
        json_t* v = json_object_get(o, k);
        return (v && json_is_number(v)) ? json_number_value(v) : def;
    }
}

SRawIntent CLlamaIntent::parseRaw(const std::string& json, int64_t deicticMs, bool deicticPlace) {
    SRawIntent r;
    json_error_t jerr{};
    json_t*      root = json_loads(json.c_str(), 0, &jerr);
    if (!root || !json_is_object(root)) {
        if (root) json_decref(root);
        r.note = "llama: raw intent JSON malformed";
        return r; // None
    }
    r.verb       = verbFromName(jstr(root, "verb"));
    r.confidence = jnum(root, "confidence", 1.0);
    r.anchor     = parseAnchorMode(jstr(root, "anchor"));
    r.sub        = jstr(root, "sub");
    r.deltaM     = jnum(root, "deltaM", 0.0);
    r.workspace  = static_cast<int>(jnum(root, "workspace", 0));
    r.note       = "llama";

    std::string target = jstr(root, "target");
    std::string app    = jstr(root, "app");
    if (r.verb == EVerb::LaunchApp)
        r.appPhrase = app.empty() ? target : app;
    else if (r.verb == EVerb::MoveWindow) {
        // A move names BOTH halves: the window rides in the free-text `app` field, the
        // destination in the enumerated `target`. A workspace destination is signalled by
        // a non-zero `workspace`, exactly as the rule backend marks it.
        r.windowPhrase = app;
        if (target != "active" && !target.empty())
            r.monitorPhrase = target;
        if (r.workspace > 0)
            r.sub = "workspace";
    } else if (r.verb == EVerb::Focus || r.verb == EVerb::Fullscreen)
        // Window verbs name an APP, and the grammar's `target` enumerates MONITORS —
        // so the free-text `app` field is the carrier. finalize resolves it against the
        // live window list, which is what keeps an invented name from actuating.
        r.targetPhrase = app.empty() ? (target == "active" ? "" : target) : app;
    else if (target != "active" && !target.empty())
        r.targetPhrase = target;   // an enumerated live name; finalize matches it exactly.

    // The model flags deixis; the WHEN comes from the transcript's located deictic.
    r.deictic        = jbool(root, "deictic") || jbool(root, "place");
    r.deicticIsPlace = jbool(root, "place") || deicticPlace;
    r.deicticWordMs  = deicticMs;

    json_decref(root);
    return r;
}

std::string CLlamaIntent::buildPrompt(const STranscript& t, const SDesktopContext& ctx,
                                      const SIntentConfig& icfg) const {
    (void)icfg;
    std::string p;
    p += "You translate a spoken command into a single JSON object controlling XR "
         "monitors in a Linux compositor. Choose exactly one verb.\n\n";
    p += "Verbs: none (not a command), clarify (ambiguous), pick (pick up / carry a "
         "monitor), place (drop/put it down), move_dist (closer/further; set deltaM "
         "negative for closer, positive for further), center, dock, undock, follow "
         "(have it follow me), anchor (world-lock or head/body), hand_input (hands "
         "on/off), monitor_view (hide/show every XR monitor; set sub to off/on/toggle), "
         "launch_app (open an app), focus (focus a running app's window), "
         "fullscreen (toggle fullscreen on a window), workspace (switch to a numbered "
         "workspace — put the number in `workspace`).\n";
    p += "For focus/fullscreen put the spoken app name in `app` (e.g. \"browser\"), not "
         "in `target`.\n";
    p += "move_window relocates ONE window (\"move the terminal to the left monitor\"): "
         "put the spoken window name in `app` and the destination monitor in `target`, or "
         "set `workspace` when the destination is a workspace.\n";
    p += "move_workspace relocates a WHOLE numbered workspace onto another output "
         "(\"move workspace 4 to this monitor\"): put the number in `workspace` and the "
         "destination monitor in `target`. A phrase whose SUBJECT is a workspace is never "
         "a move_window.\n";
    p += "create_monitor makes a NEW XR monitor (\"create a monitor here\"): leave "
         "`target` empty — the daemon mints the name — and set place=true for \"here\".\n";
    p += "target MUST be one of the monitor names below, or \"active\" for "
         "this/that/selected, or \"\".\n";
    p += "Set deictic=true when the user says \"this\"/\"that\"; place=true for "
         "\"here\"/\"there\".\n\n";
    p += ctx.digest(icfg.gaze.samples > 0 ? 16 : 16);
    p += "\nUser said: \"" + t.text + "\"\n";
    p += "JSON:";
    return p;
}

// ----------------------------------------------------------------------------
// Model-backed parts.
// ----------------------------------------------------------------------------
#ifdef HAVE_LLAMA

#include "common.h"
#include "llama.h"
#include "sampling.h"

struct CLlamaIntent::Impl {
    common_init_result_ptr init;
    llama_model*           model = nullptr;
    llama_context*         ctx   = nullptr;
    bool                   ok    = false;
};

CLlamaIntent::CLlamaIntent() : m_impl(std::make_unique<Impl>()) {}
CLlamaIntent::~CLlamaIntent() = default;

bool CLlamaIntent::loaded() const { return m_impl && m_impl->ok; }

bool CLlamaIntent::load(const SLlamaParams& params, std::string& err) {
    m_params = params;
    if (params.modelPath.empty()) {
        err = "intent.model is empty (GGUF path required for the llama backend)";
        return false;
    }
    llama_backend_init();

    common_params cp;
    cp.model.path          = params.modelPath;
    cp.n_ctx               = params.nCtx;
    cp.n_batch             = params.nCtx;
    cp.cpuparams.n_threads = params.nThreads;
    cp.n_predict           = params.nPredict;
    cp.warmup              = false;

    m_impl->init = common_init_from_params(cp);
    if (!m_impl->init || !m_impl->init->model() || !m_impl->init->context()) {
        err = "failed to load GGUF model: " + params.modelPath;
        return false;
    }
    m_impl->model = m_impl->init->model();
    m_impl->ctx   = m_impl->init->context();
    m_impl->ok    = true;
    Log::log(Log::INFO, "llama intent backend loaded: {}", params.modelPath);
    return true;
}

SAction CLlamaIntent::resolve(const STranscript& t, const SDesktopContext& ctx,
                              const GazeQueryFn& gazeQuery, const SIntentConfig& icfg) {
    SAction fail;
    fail.verb      = EVerb::None;
    fail.utterance = t.text;
    if (!loaded()) {
        fail.note = "llama backend not loaded";
        return fail;
    }

    const llama_vocab* vocab = llama_model_get_vocab(m_impl->model);

    // Per-utterance grammar over the LIVE monitor names.
    std::string grammar = Grammar::buildIntentGrammar(ctx.monitorNames());
    common_params_sampling sp;
    sp.temp    = static_cast<float>(m_params.temperature);
    sp.grammar = common_grammar(COMMON_GRAMMAR_TYPE_USER, grammar);
    if (m_params.temperature <= 0.0)
        sp.top_k = 1; // greedy

    common_sampler* smpl = common_sampler_init(m_impl->model, sp);
    if (!smpl) {
        fail.note = "failed to init grammar sampler";
        return fail;
    }

    // Fresh context each call.
    llama_memory_clear(llama_get_memory(m_impl->ctx), true);

    std::string prompt = buildPrompt(t, ctx, icfg);
    std::vector<llama_token> toks = common_tokenize(m_impl->ctx, prompt, /*add_special=*/true, /*parse_special=*/true);

    std::string out;
    bool        okGen = true;
    llama_batch batch = llama_batch_get_one(toks.data(), toks.size());
    for (int generated = 0; generated < m_params.nPredict; generated++) {
        if (llama_decode(m_impl->ctx, batch)) {
            okGen = false;
            break;
        }
        llama_token id = common_sampler_sample(smpl, m_impl->ctx, -1);
        common_sampler_accept(smpl, id, /*is_generated=*/true);
        if (llama_vocab_is_eog(vocab, id))
            break;
        out += common_token_to_piece(m_impl->ctx, id);
        batch = llama_batch_get_one(&id, 1);
        // Early stop: the grammar-completed object closes with a '}'.
        if (!out.empty() && out.back() == '}')
            break;
    }
    common_sampler_free(smpl);

    if (!okGen) {
        fail.note = "llama decode failed";
        return fail;
    }

    SDeicticHit d = findDeictic(t);
    SRawIntent  raw = parseRaw(out, d.ms, d.isPlace);
    // The TRANSCRIPT is authoritative for deixis: if the user said "this"/"that", the
    // reference is gaze-resolved at word time, NOT whatever monitor the model guessed
    // (the grammar lets it pick any enumerated name; for a deictic that guess is
    // unreliable). Clear the model's target so the shared finalize uses the gaze ring.
    if (d.found && !d.isPlace) {
        raw.deictic      = true;
        raw.targetPhrase.clear();
    }
    // The grammar constrains deltaM to be a number, not a sane one — validate the model's
    // push/pull against the utterance's own direction words + the magnitude rule (live 3B
    // runs emitted +100 for "closer", then -1.00 m for a bare "move closer"). finalizeAction
    // applies the same function to EVERY backend; doing it here too keeps the raw intent
    // that gets logged honest, and sanitizeDeltaM is idempotent.
    if (raw.verb == EVerb::MoveDist)
        raw.deltaM = sanitizeDeltaM(raw.deltaM, t.text, icfg.distanceStep);
    SAction a = finalizeAction(raw, t, ctx, gazeQuery, icfg);
    if (a.note.empty())
        a.note = "llama";
    Log::log(Log::DEBUG, "llama raw intent: {}", out);
    return a;
}

#else  // !HAVE_LLAMA — stub so the daemon links and falls back to the rule backend.

struct CLlamaIntent::Impl {};
CLlamaIntent::CLlamaIntent() : m_impl(nullptr) {}
CLlamaIntent::~CLlamaIntent() = default;
bool CLlamaIntent::loaded() const { return false; }
bool CLlamaIntent::load(const SLlamaParams&, std::string& err) {
    err = "built without llama.cpp (HAVE_LLAMA off) — using the rule backend";
    return false;
}
SAction CLlamaIntent::resolve(const STranscript& t, const SDesktopContext&,
                              const GazeQueryFn&, const SIntentConfig&) {
    SAction a; a.verb = EVerb::None; a.utterance = t.text; a.note = "llama backend not built";
    return a;
}

#endif
