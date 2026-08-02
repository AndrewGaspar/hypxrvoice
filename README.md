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
                   │ PCM reaches the VAD/ASR ONLY when armed or PTT-held (the gate).
                   │ The stream itself is HELD open in a headset session; gated frames
                   │ go nowhere but a ~1 s pre-roll ring, spliced onto the next window.
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
             + in-headset HUD (hypxrhud D-Bus client) + terse TTS  [WP-V5/H8]
                   ┊
   ─ ─ ─ ─ ─ ─ ─ ─ ┊ ─ ─ ─ ─ ─  later work packages (seams only)  ─ ─ ─ ─ ─ ─ ─ ─
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

Each transcript is turned into a **typed, closed-schema command** (`SAction`, 18 verbs)
and then an allowlisted `hyprctl` plan. Thirteen verbs drive the XR layer (pick, place,
move, center, dock/undock, follow, anchor, hand input, launch, **create_monitor** —
"create a monitor here"); five drive plain Hyprland window management — **focus**
("focus the browser"), **fullscreen** ("make this window fullscreen"), **workspace**
("workspace three"), **move_window** ("move terminal to the left monitor", "send the
browser to workspace 3"), and **move_workspace** ("move workspace 4 to this monitor").
The window verbs are non-destructive and reversible, so they are permitted by default
(`executor.allow_window`).

Two safety rules earned the hard way on the first non-dry-run round:

- **A deixis designates a point in FRONT of you.** A gaze sample gives a head *origin*
  and a direction; `intent.place_distance_m` (default 1.3 m) projects along it and
  `intent.place_min_distance_m` (0.5 m) is a hard floor on every candidate point. Passing
  the head origin through was what dropped a monitor *at the HMD*.
- **An unmatched window reference never actuates.** A window phrase with content in it
  that matches nothing live is a Clarify, never a fall-through to the focused window; and
  a phrase whose subject is a *workspace* is never a window reference at all. In the
  workspace **number slot** only, Whisper's predictable homophones are accepted
  (`for`/`forward`→4, `to`/`too`→2, `ate`→8, `won`→1, `tree`/`free`→3) — that is how
  "move workspace forward to this monitor" reaches `move_workspace 4`.

- **Desktop-context snapshot** at utterance time — `hyprctl monitors -j` + `clients -j` +
  `-j openxr status` (all read-only) — enumerates live monitor names and the apps on
  them, so "the coding monitor" / "youtube" resolve to a real `XR-*` name.
- **Intent backend** — `backend = "rule"` (deterministic keyword grammar; default, no
  model) or `backend = "llama"` (a local GGUF, **GBNF-constrained** so it can only emit
  the schema and can only name an enumerated monitor). Both feed one shared resolver.
- **Deixis** — "pick **this** up" / "place it **here**" is resolved at the timestamp of
  the deictic *word* via `hyprctl -j openxr gaze at <ms>`, over a lead-shifted stability
  window (gaze leads speech; ASR timestamps are noisy). One deictic per utterance.
- **Window references** — "the browser" / "the editor" / "the terminal" resolve through a
  small **closed** generic-noun table intersected with the LIVE window list, so a spoken
  noun can only ever select an app that is actually running. The chosen window is
  dispatched at its **address**, never `class:` — Hyprland reads a class as a regex.
- **Spatial monitor references** — "the **left** monitor" is a claim about the layout,
  not about a name, so it resolves by x coordinate from `monitors -j` (leftmost /
  rightmost). Two monitors sharing the extreme x is genuinely ambiguous and asks rather
  than picking; one monitor cannot answer at all. A move therefore never lands somewhere
  the user did not name.
- **Executor** — a pure map from `SAction` to a plan of `hyprctl openxr …` /
  `dispatch exec|focuswindow|focusmonitor|fullscreen|workspace|movewindow|movetoworkspace …`
  argv, behind a **STRICT allowlist**: a closed dispatcher set with each argument *shape*
  checked (an address must be hex, a workspace must be 1–99, fullscreen must be 0/1, a
  move destination must be `mon:<name>` over a boring charset), and transcript text is
  never interpolated into a command line. **`executor.dry_run` defaults true** — it logs the
  exact argv and actuates nothing until you flip it off.

Two spoken interactions need compositor verbs that don't exist yet (targeted grab,
place-at-pose); the executor uses documented approximations behind capability flags and
the precise proposed verbs live in [`docs/COMPOSITOR-GAPS.md`](docs/COMPOSITOR-GAPS.md).

### Feedback tier — in-headset HUD + terse TTS (WP-V5, HUD migrated in WP-H8)

Every recognised command flows through one sink, `Feedback::emitAction(action, plan,
cfg)`, which drives three channels:

- **In-headset HUD** — a head-locked panel showing the **live transcript** while listening
  and the **parsed action** (verb + target, how the target resolved, a confidence bar,
  `approx`/`dry-run` badges, or a clarify question + candidates) as your **veto window**
  before/as it executes. As of **WP-H8** hypxrvoice does **not** render this itself: it is
  a pure **D-Bus client** of the shared **[hypxrhud](../hypxrhud)** daemon
  (`io.github.andrewgaspar.hypxrhud1`), which owns the one OpenXR overlay session, the
  rendering, the geometry, and the fades for the whole HypXRland ecosystem. hypxrvoice maps
  each `SHudView` onto the daemon's `a{sv}` panel props (`HudClient::hudPropsFromView`) and
  pushes it to a named **slot** (`voice` by default):
  `onListeningStart` → `CreatePanel({slot:"voice", …})` (keeps the returned id); each
  per-word transcript → `UpdatePanel(id, {lines, confidence})` **fire-and-forget**
  (`NO_REPLY_EXPECTED`); a recognised action/clarify updates the same panel; mic-close
  without a command → `DismissPanel(id)`. The daemon is **bus-activated**, so the first
  create starts it if its unit is installed. hypxrvoice links **no** OpenXR/EGL/GBM/stb —
  only `sd-bus`.
- **Terse TTS** — spoken confirmations for what you *can't* see: refusals and clarify
  questions (`tts = "errors"`, the default), optionally every action (`"all"`). Short
  phrasings ("moving XR-code", "which firefox?", "can't, no such monitor"). **No
  ONNX/piper** (the WP-V2 constraint): it shells out to the `espeak-ng` binary if on PATH,
  else TTS is cleanly disabled. TTS is voice-owned and stays in this repo. Point
  `audio.source` at a `pipewire-module-echo-cancel` source so the assistant doesn't hear
  its own voice.
- **notify-send** — the fallback when the HUD isn't carrying the message: the hypxrhud
  daemon is **absent/unreachable**, its `RuntimeState` is not `"live"` (no headset/runtime
  donned), or `hud = false`. hypxrvoice watches `RuntimeStateChanged` + `NameOwnerChanged`
  to know, so it never polls; a single logged note marks the degrade.

**A rejected window is never silent.** A transcript the parser can't use, or a window with
no speech in it at all, used to *hide* the HUD — which looks exactly like a dead
microphone, and gave no clue whether audio, ASR, or the grammar was at fault. Both now
raise a brief **rejection panel** (~2.5 s): the transcript echoed back verbatim above
"didn't catch a command", or "didn't hear anything" when nothing was heard. The outcome
also lands in `hypxrvoicectl status` as `lastOutcome`
(`ok` / `unparsed` / `no-speech` / `asr-unavailable`) alongside `lastText`. A wake-word
window that simply wasn't addressed to you stays quiet — that is what the wake phrase is
for; only an *explicitly requested* (PTT) window always answers.

The view model + phrasing + colour roles stay a **pure library** (`HudModel`) and the
`SHudView → props` mapping (`HudClient`) is pure and unit-tested with no bus. The HUD's
actual pixels are hypxrhud's concern — review them there with `hypxrhud --preview`.

See **[Live in-headset validation](#live-in-headset-validation-wp-v5--wp-h8)** for the
on-headset checklist.

### Activation state machine

`GATED_KEYBOARD → ARMED_WAKEWORD → LISTENING → TRANSCRIBING`, driven by the compositor
signal + PTT. Mirrors the compositor's `hand_input=auto`: at the keyboard it is PTT-only;
away / headset donned it arms the wake word. If the compositor or its `openxr` status
section is absent, it degrades to wake-armed (configurable).

### Persistent capture + the pre-roll ring

Opening the PipeWire stream at the PTT press cost **~0.5–1 s** — the stream connect plus a
`wivrn.source` resume from SUSPENDED, which makes the WiVRn server ask the headset client
to start its mic over the network. You are already speaking during that hole, so every
transcript arrived missing its leading verb and short commands never arrived at all.

The stream is therefore **held connected** across windows whenever a headset is present
(`capture.hold`, default on). Two separate things now:

| | meaning | `status` field |
|---|---|---|
| stream connected | PCM is being pulled from the source | `micOpen`, `captureState` |
| **gate open** | frames reach the VAD / ASR / wake-word tiers | `captureActive` |
| held | connected with the gate shut | `captureHeld` |

**The privacy invariant is the gate, not the stream.** While the gate is shut the frames
are written to exactly one place: a rolling `capture.preroll_ms` (default 1 s ≈ 32 kB)
pre-roll ring in RAM. They are never analysed, never transcribed, never persisted, and
never leave the process. When a window opens the ring is spliced onto the front of it, so
speech that began *before* the press is transcribed in full. Out of the headset the stream
is still opened per window (a desk source resumes locally and fast, so holding it buys
nothing). `capture.hold = false` restores per-window streams everywhere.

### The onset back-pad

Holding the stream was necessary but not sufficient: leading words kept disappearing
("browser." for "focus the browser", "3." for "workspace three"). The second cause was in
the segmenter. The VAD's retention ring is fed *every* idle frame — including the
`vad.start_ms` of voiced audio that declares onset — and onset is back-dated to the first
of those frames. So `vad.pre_roll_ms` alone left only `pre_roll_ms - start_ms` of audio in
front of the onset instant: **150 ms** at the shipped defaults, half of what its name
implied. A first syllable that is quiet — an unvoiced fricative, or any source whose gain
is still ramping when you start talking — lives in that gap and was dropped despite having
been in the buffer the whole time.

`vad.onset_backpad_ms` (default 300) states the guarantee directly: the ring is sized as
`max(pre_roll_ms, onset_backpad_ms + start_ms)`, and every emitted segment carries at least
that much audio ahead of its onset. `hypxrvoiced` logs the achieved back-pad for every
segment, so the journal alone distinguishes "cut short" from "never arrived".

### A push-to-talk window is transcribed WHOLE

The back-pad fixed the *retention*; live round 3 showed the remaining loss was the
*onset criterion itself*. `vad.start_ms` demanded that many **consecutive** voiced frames,
and no real word delivers that — "focus" has a stop gap before its /k/, "workspace" one
before its /sp/. In one six-utterance round that cost two commands outright: "focus the
browser" onset at the word *browser* (whisper heard "The browser."), and "workspace three"
produced no segment at all (`no-speech`) with ~400 ms of speech plainly in the window wav.

Two changes, both driven by measurements off those dumps:

- **`vad.gap_tolerance_ms`** (default 100) lets the onset run *pause* across a short
  unvoiced dip instead of resetting. Only voiced frames count toward `start_ms`, so the
  gate is no looser on noise. Replaying the live window, this alone moves onset from
  2.82 s back to 2.04 s — from "browser" to "focus".
- **`capture.ptt_whole_window`** (default true) stops gating the PTT path on onset at all.
  A press is an explicit declaration that speech is coming, so the utterance handed to
  whisper *is* the window: pre-roll splice through release, minus an obviously-dead tail,
  capped at `vad.max_utterance_ms`. The VAD keeps two jobs — endpointing early (so a short
  command need not wait out the toggle) and a deliberately forgiving "was there anything
  in this window at all" verdict (`vad.presence_ms`) that gates *below* the detector's own
  threshold. When in doubt it transcribes: whisper returning noise and the intent tier
  rejecting it is a fine outcome; silently eating speech is not.

The wake-word path is unchanged apart from the gap tolerance — it has no press to trust,
so it still segments on onset.

`vad.energy_threshold` also dropped 0.012 → **0.006**: the Quest/WiVRn mic is unprocessed,
its measured ambient across all seven dumped windows is ~0.0002 RMS and its speech peaks
only ~0.03. The adaptive floor, not the fixed one, is what protects a hot source.

### Capture forensics (`debug.dump_audio_dir`, off by default)

When a leading word goes missing there are two very different causes — the source never
sent it (headset-mic gating/AGC), or we cut it — and they produce identical transcripts.
Setting `debug.dump_audio_dir` writes, per capture window, the **full window audio**
(pre-roll splice included), **each segment handed to whisper**, and a sidecar `.txt` with
the splice point and every VAD boundary. If the word is inaudible in the window wav it is
the source; if it is audible there but absent from the segment wav it is us. Disk use is
bounded to the newest `dump_audio_keep` windows (default 50).

**This writes microphone audio to disk**, which is why it is off by default and why the
daemon logs a standing warning for as long as it is set. Unset it when the round is over.

**Trade-off:** a held stream keeps `wivrn.source` running, which keeps the headset mic
streaming — a small continuous battery cost on the headset. Wake-word mode already did
this; PTT mode now does too while donned.

Stream health is supervised: a source that errors, vanishes (WiVRn disconnect), or goes
silent for 10 s while gated is reconnected with exponential backoff (1 s → 30 s cap)
rather than retried at the tick rate. A reconnect never interrupts an open window.

## Build

```sh
cmake -B build
cmake --build build
```

### Requirements (system libraries, via pkg-config)

- `libpipewire-0.3` + `libspa-0.2` — microphone capture
- `sndfile` — WAV/audio decode for `--oneshot`
- `jansson` — parse `hyprctl openxr status -j`
- `libsystemd` — the **sd-bus client** of the shared **hypxrhud** HUD daemon (WP-H8). This
  is the only new HUD dependency; hypxrvoice links **no** OpenXR/EGL/GBM/stb.
- `libnotify` / `notify-send` at runtime — toasts (optional; silently skipped if absent)
- **[hypxrhud](../hypxrhud)** at runtime (optional) — the in-headset HUD. Install + enable
  its user service (`systemctl --user enable --now hypxrhud.service`, or rely on D-Bus bus
  activation) for the HUD to appear; without it hypxrvoice degrades to notify-send. The
  OpenXR/EGL/GBM/stb + the bundled font that used to live here now live in hypxrhud.
- **`espeak-ng`** binary at runtime (optional) — terse TTS; cleanly disabled if not on
  PATH. No ONNX/piper runtime is used or linked.

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
+ executor (honours `executor.dry_run`), or `--ptt` to treat the file as **one push-to-talk
window** (whole-window transcription) — which is how a dumped capture window is replayed
against the live grammar:

```sh
hypxrvoiced --oneshot ~/dumps/20260727-000946.288-w15327740-window.wav --ptt --intent
```

To drive the intent tier directly on a text utterance (no audio):

```sh
hypxrvoiced --intent-text "move the coding monitor closer"
```

## Configuration

`~/.config/hypxrvoice/config.toml` (see `examples/config.toml` for the annotated full
set). Sections: `[activation]` (mode auto/ptt/wake, fallback), `[audio]` (source,
sample rate), `[capture]` (`hold`, `preroll_ms` — see
[Persistent capture](#persistent-capture--the-pre-roll-ring)), `[vad]` (thresholds),
`[wake]` (phrase, backend), `[asr]` (model path,
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
matrix with a mocked hyprctl, the pure llama grammar/parse/prompt, plus the feedback tier —
HUD view-model builders and the pure `SHudView → hypxrhud panel-props` mapping
(`HudClient::hudPropsFromView`, WP-H8), and TTS phrase selection across modes) is unit
tested and needs no model. A second test binary (`hypxrvoice_hud_dbus_tests`) is a **D-Bus
integration test**: it spawns the **real** hypxrhud daemon (`--no-xr`) on a **private**
session bus via `dbus-run-session` and drives `CHudClient` through a listening → update →
dismiss round-trip (asserted via the daemon's `PanelCount`/`GetCapabilities`), plus the
daemon-absent → notify-send fallback (mocked sink). It never touches your real session bus
and self-skips if `dbus-run-session` or the hypxrhud binary is absent (point
`-DHYPXRHUD_BIN=…` or the `HYPXRHUD_BIN` env at it). The `oneshot` case runs the real
whisper pipeline over the bundled public-domain JFK clip and asserts per-word monotonic
timestamps; it is skipped (passes trivially) when no model is present — set
`HYPXRVOICE_TEST_MODEL` or drop a model in `models/` to exercise it.

## Live in-headset validation (WP-V5 / WP-H8)

The HUD's logic and the props mapping are covered offline (unit tests + the D-Bus
integration test), but the actual in-headset composition is **hypxrhud's** and is verified
on a headset there. hypxrvoice's job is just to push the right panels. To validate the
end-to-end path:

1. **Install + run hypxrhud.** Build the sibling [hypxrhud](../hypxrhud), install its D-Bus
   activation + user unit, and either `systemctl --user enable --now hypxrhud.service` or
   let the first `CreatePanel` bus-activate it. Its own config
   (`~/.config/hypxrhud/hypxrhud.toml`) owns the HUD **geometry, GPU, overlay z, opacity,
   and the rise/hold/fade envelope** — the keys that used to be `feedback.hud_*` here.
   Point the `voice` slot pose/size there; ensure its overlay `z` differs from HypXRland's
   `openxr:overlay_z`. There is one XR runtime per box, so this runs alongside HypXRland,
   not a second session.
2. **Enable the HUD in hypxrvoice.** Set `feedback.hud = true` (and, if you re-slot it,
   `feedback.hud_slot`). `hud`/`hud_slot`/`tts*` apply on `reload`.
3. **Full path:** run `hypxrvoiced`, PTT a command, and check in-headset:
   - the **listening** panel appears on PTT and shows the partial transcript;
   - the **action** panel replaces it with the verb+target, a confidence bar, and the
     `approx`/`dry-run` badges, then **fades** (hypxrhud applies the envelope hypxrvoice
     forwards as `hold_ms`/`fade_ms` props);
   - a **clarify** utterance ("open firefox" with two firefoxes) shows the candidate list.
   Tune legibility/comfort (pose/size/opacity) in **hypxrhud's** config.
4. **TTS:** install `espeak-ng`; with `tts = "errors"` a refusal speaks "can't, …" and a
   clarify speaks the question; `tts = "all"` also confirms successes. Point
   `audio.source` at an echo-cancel source so it doesn't re-trigger on its own voice.
5. **Degradation:** stop hypxrhud (or undon the headset so its `RuntimeState` leaves
   `"live"`) and confirm hypxrvoice logs *HUD daemon unavailable … using notifications
   only* once and falls back to `notify-send`, then recovers when hypxrhud returns (it
   watches `NameOwnerChanged` + `RuntimeStateChanged`).

**hypxrvoice config keys:** `hud` (on/off), `hud_slot` (default `voice`); `tts`
(off/errors/all), `tts_voice`, `tts_rate`. **Moved to hypxrhud config:** `hud_pose`,
`hud_size`, `hud_opacity`, `hud_hold_ms`, `hud_fade_ms`, `hud_z`, `hud_gpu` (still accepted
in `config.toml` with a one-line "moved" warning so old configs keep loading).

## What is NOT here (later work packages)

- **WP-V6 — cloud escalation.** Claude Messages API tool-use for hard phrasings; local
  remains the default and the only network-free path.

## License

BSD 3-Clause. See `LICENSE`. Bundled `tests/assets/jfk.wav` is a public-domain excerpt of
John F. Kennedy's 1961 inaugural address (via the whisper.cpp samples).
