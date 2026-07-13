# hypxrvoice

**Local-first voice control for [HypXRland](https://github.com/hyprwm/Hyprland)** — a
standalone companion daemon (sibling to `hypxrpaper` / `hypxrkeys`) that turns spoken
commands into HypXRland monitor manipulations and app launches. It speaks PipeWire on
one side and the Hyprland sockets on the other; **no voice logic ever enters the
compositor.**

The whole design is a **cascade** whose point is to keep the microphone shut and the
cloud bill at zero until a real command is spoken (see
`../Hyprland/docs/openxr/research/VOICE-CONTROL.md`, Proposal B). This repository
implements **WP-V2 (transcript tier)** and **WP-V4 (intent tier + command executor)** —
everything from audio capture through a timestamped transcript, then a safe, allowlisted
compositor command. An in-headset HUD, TTS, and cloud escalation are later work packages;
the code leaves clean seams for them.

## Cascade architecture

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ TIER 0 — activation gate            [IMPLEMENTED]                          │
   │   hyprctl openxr status -j @1Hz  ─▶  at keyboard  → GATED_KEYBOARD (PTT)   │
   │   PTT via hypxrvoicectl          ─▶  away/donned  → ARMED_WAKEWORD         │
   │   (compositor absent → wake-armed fallback)                               │
   └───────────────┬───────────────────────────────────────────────────────────┘
                   │ mic PCM opened ONLY when armed or PTT-held (privacy invariant)
                   ▼
   PipeWire ─▶ [ energy VAD ] ─▶ speech segment (+ CLOCK_MONOTONIC onset)   [IMPLEMENTED]
   wivrn.source /               │
   default source              ▼
   ┌───────────────────────────────────────────────────────────────────────────┐
   │ TIER 1 — wake word   [IMPLEMENTED: vad-transcribe backend]                  │
   │   armed speech is transcribed, the wake phrase matched + stripped           │
   │   (SEAM: openwakeword ONNX pre-ASR gate = WP-V7)                            │
   ├───────────────────────────────────────────────────────────────────────────┤
   │ TIER 2 — streaming ASR   [IMPLEMENTED: whisper.cpp + DTW word timestamps]   │
   │   → STranscript { text, onsetMs, words[{text,startMs,endMs}] }              │
   └───────────────┬───────────────────────────────────────────────────────────┘
                   ▼
                   ▼
   ┌───────────────────────────────────────────────────────────────────────────┐
   │ TIER 3 — intent + executor   [IMPLEMENTED: WP-V4]                           │
   │   transcript ─▶ desktop-context snapshot (monitors/clients/openxr, read-only)│
   │             ─▶ intent (rule grammar | llama.cpp GBNF) ─▶ typed SAction       │
   │             ─▶ executor: STRICT-allowlist hyprctl argv (dry-run by default)  │
   │   deixis ("this"/"here") resolved at WORD time via `openxr gaze at <ms>`     │
   └───────────────┬───────────────────────────────────────────────────────────┘
                   ▼
   feedback: stdout JSON + log + notify-send            [IMPLEMENTED]
                   ┊
   ─ ─ ─ ─ ─ ─ ─ ─ ┊ ─ ─ ─ ─ ─  later work packages (seams only)  ─ ─ ─ ─ ─ ─ ─ ─
   in-headset HUD lane + piper TTS confirmations                 [WP-V5]
   cloud reasoning escalation (Claude tool-use)                  [WP-V6]
```

### The spatial-anchoring contract

Every transcript carries a **speech-onset timestamp in CLOCK_MONOTONIC milliseconds**
(`onsetMs`), captured at VAD onset (or PTT press). Each word additionally carries
absolute `{startMs, endMs}`. This is the substrate for deictic commands: the downstream
intent tier resolves *"move **this** monitor over **here**"* by asking the compositor
`hyprctl openxr gaze at <ms>` for the head pose at each deictic word's timestamp. Word
timestamps come from whisper's **DTW token-alignment** when an alignment-heads preset is
known for the model (base/small/tiny .en all have one); otherwise it falls back to the
heuristic t0/t1 timestamps. HypXRland's compositor clock is CLOCK_MONOTONIC, so these
values are directly comparable.

## Components

- **`hypxrvoiced`** — the long-lived daemon. Single-threaded state model (state machine,
  VAD, ASR, control, compositor poll all on one epoll loop); only PipeWire's RT callback
  runs off-thread and merely queues samples.
- **`hypxrvoicectl`** — thin control CLI over an `AF_UNIX` socket in
  `$XDG_RUNTIME_DIR/hypxrvoice/`: `ptt start|stop|toggle`, `status`, `reload`.

### Intent tier + executor (WP-V4)

Each transcript is turned into a **typed, closed-schema command** (`SAction`, 12 verbs)
and then an allowlisted `hyprctl` plan:

- **Desktop-context snapshot** at utterance time — `hyprctl monitors -j` + `clients -j` +
  `-j openxr status` (all read-only) — enumerates live monitor names and the apps on
  them, so "the coding monitor" / "youtube" resolve to a real `XR-*` name.
- **Intent backend** — `backend = "rule"` (deterministic keyword grammar; default, no
  model) or `backend = "llama"` (a local GGUF, **GBNF-constrained** so it can only emit
  the schema and can only name an enumerated monitor). Both feed one shared resolver.
- **Deixis** — "pick **this** up" / "place it **here**" is resolved at the timestamp of
  the deictic *word* via `hyprctl -j openxr gaze at <ms>`, over a lead-shifted stability
  window (gaze leads speech; ASR timestamps are noisy). One deictic per utterance.
- **Executor** — a pure map from `SAction` to a plan of `hyprctl openxr …` /
  `dispatch exec -- …` argv, behind a **STRICT allowlist** (closed verb set; transcript
  text is never interpolated into a command line). **`executor.dry_run` defaults true** —
  it logs the exact argv and actuates nothing until you flip it off.

Two spoken interactions need compositor verbs that don't exist yet (targeted grab,
place-at-pose); the executor uses documented approximations behind capability flags and
the precise proposed verbs live in [`docs/COMPOSITOR-GAPS.md`](docs/COMPOSITOR-GAPS.md).

### Activation state machine

`GATED_KEYBOARD → ARMED_WAKEWORD → LISTENING → TRANSCRIBING`, driven by the compositor
signal + PTT. The **mic PCM stream is opened only in armed/listening states** — a
process-level privacy invariant (observable in `pw-top`/`wpctl`), not a promise. Mirrors
the compositor's `hand_input=auto`: at the keyboard it is PTT-only; away / headset donned
it arms the wake word. If the compositor or its `openxr` status section is absent, it
degrades to wake-armed (configurable).

## Build

```sh
cmake -B build
cmake --build build
```

### Requirements (system libraries, via pkg-config)

- `libpipewire-0.3` + `libspa-0.2` — microphone capture
- `sndfile` — WAV/audio decode for `--oneshot`
- `jansson` — parse `hyprctl openxr status -j`
- `libnotify` / `notify-send` at runtime — toasts (optional; silently skipped if absent)

Vendored under `subprojects/` (pinned shallow submodules): **whisper.cpp** (ASR) and
**llama.cpp** (the optional local-LLM intent backend). Clone with submodules, or
`git submodule update --init --depth 1`. The llama.cpp pin is chosen so its bundled ggml
matches whisper.cpp's (they share one ggml target); disable it with
`-DHYPXRVOICE_LLAMA=OFF` to build the rule backend only. `doctest` (tests) is vendored as
a single header in `third_party/`.

### Models

ASR models are **not** committed (large binaries). Fetch one and point the config at it:

```sh
scripts/fetch-models.sh base.en          # ~142 MB, recommended default
# then set in ~/.config/hypxrvoice/config.toml:
#   [asr]
#   model = "/path/to/models/ggml-base.en.bin"
```

The research doc favors `small` (~466 MB) for tougher phrasing; `tiny.en` (~75 MB) is
fastest/least accurate. `hypxrvoicectl status` reports `asrLoaded:false` with a clear log
line when the model is missing.

For the **llama intent backend** (optional), fetch the pinned 3B-class GGUF and point
`[intent]` at it:

```sh
scripts/fetch-models.sh intent           # Qwen2.5-3B-Instruct-Q4_K_M.gguf (~1.9 GB)
# then:  [intent]
#        backend = "llama"
#        model = "/path/to/models/Qwen2.5-3B-Instruct-Q4_K_M.gguf"
```

The `rule` backend (default) needs no model. On this box (CPU), the llama backend
resolves a command in ~2.9 s; the rule backend in ~15 ms.

## Run

```sh
# 1. Configure
mkdir -p ~/.config/hypxrvoice && cp examples/config.toml ~/.config/hypxrvoice/config.toml
$EDITOR ~/.config/hypxrvoice/config.toml       # set [asr] model = "..."

# 2. Run the daemon (or install the systemd user unit — see systemd/)
./build/hypxrvoiced

# 3. Bind a push-to-talk key. DO NOT paste examples/hyprland-binds.conf blindly —
#    validate every bind against your Omarchy/Hyprland config first (hyprctl binds),
#    exactly like the compositor repo's validated-binds lesson.
#    e.g.  bind = SUPER, V, exec, hypxrvoicectl ptt start
#          bindr = SUPER, V, exec, hypxrvoicectl ptt stop

# 4. Inspect
hypxrvoicectl status
hypxrvoicectl reload      # after editing the config
```

### Offline / test path

```sh
hypxrvoiced --oneshot tests/assets/jfk.wav --model models/ggml-base.en.bin
```

runs the full VAD → ASR → transcript pipeline on an audio file (no microphone) and prints
each utterance as a JSON line with `onsetMs` and per-word timestamps. This is the path the
acceptance test drives. Add `--intent` to also run each transcript through the intent tier
+ executor (honours `executor.dry_run`). To drive the intent tier directly on a text
utterance (no audio):

```sh
hypxrvoiced --intent-text "move the coding monitor closer"
```

## Configuration

`~/.config/hypxrvoice/config.toml` (see `examples/config.toml` for the annotated full
set). Sections: `[activation]` (mode auto/ptt/wake, fallback), `[audio]` (source,
sample rate), `[vad]` (thresholds), `[wake]` (phrase, backend), `[asr]` (model path,
language, threads), `[intent]` (backend rule/llama, model, deixis window/lead),
`[executor]` (**dry_run default true**, allow_launch, capability flags), `[apps]`
(launch allowlist), `[feedback]`, `[compositor]` (poll). Hot-reload with
`hypxrvoicectl reload`.

## Tests

```sh
cmake --build build --target hypxrvoice_tests
./build/hypxrvoice_tests            # or: ctest --test-dir build
```

Pure logic (activation state machine, config parsing, VAD segmentation + timestamp
bookkeeping, wake-word matching, transcript JSON, compositor-status parsing, plus the
WP-V4 tier — context snapshot + semantic resolution, gaze/deixis stability window,
executor allowlist, rule-intent grammar, the full transcript→context→intent→executor
matrix with a mocked hyprctl, and the pure llama grammar/parse/prompt) is unit tested and
needs no model. The `oneshot` case runs the real whisper pipeline over the
bundled public-domain JFK clip and asserts per-word monotonic timestamps; it is skipped
(passes trivially) when no model is present — set `HYPXRVOICE_TEST_MODEL` or drop a model
in `models/` to exercise it.

## What is NOT here (later work packages)

- **WP-V5 — feedback tier.** In-headset HUD overlay lane + piper TTS. Seam: the
  `Feedback::emitAction(action, plan, cfg)` sink (V4) — it already carries the phrasing,
  the resolved target + how it resolved, a confidence in [0,1], the clarify
  question/candidates, and the executor plan (dry-run/live, exact/approximated). AEC
  (`pipewire-module-echo-cancel`) matters here (so
  the assistant doesn't hear its own TTS); the capture already takes whatever source it is
  pointed at, so pointing `audio.source` at an echo-cancel source is the wiring.
- **WP-V6 — cloud escalation.** Claude Messages API tool-use for hard phrasings; local
  remains the default and the only network-free path.

## License

BSD 3-Clause. See `LICENSE`. Bundled `tests/assets/jfk.wav` is a public-domain excerpt of
John F. Kennedy's 1961 inaugural address (via the whisper.cpp samples).
