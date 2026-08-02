# Deixis and target semantics

What "this", "here" and "active" mean, per verb, and where each answer is produced.

Written for round 8, after two live misfires that had the same root cause: a verb path
that quietly substituted *the compositor's idea of the current monitor* for *the monitor
the user was looking at*. The two are almost never the same thing in a headset.

---

## 1. The contract

**A deictic is resolved from GAZE, at the timestamp of the WORD that carried it.**

Not at execution time, not at parse time, not at utterance end. Whisper decode alone puts
1.5–3 s between the word and the action, and the head has moved on. The compositor keeps
a 90 Hz pose ring precisely so we can ask *backwards*: `hyprctl -j openxr gaze at <ms>`.
`deixis_lead_ms` shifts the query slightly earlier (gaze leads speech by 200–600 ms and
ASR word timestamps carry ±100–200 ms of their own), and `deixis_window_ms` /
`deixis_samples` vote across a small window so one saccade cannot hijack the reference.

Three rules follow, and they are the ones round 8 enforces:

1. **Every deictic goes through the ring.** No verb may special-case its way past it.
2. **The window decides *which*, never *when*.** The vote spans the window; the POSE
   comes from the sample nearest the (lead-shifted) word.
3. **A deictic that resolves to nothing is a Clarify, not a fallback.** The user said
   "this", so they were looking at something specific, and every fallback available
   (pointer hover, sticky selection, keyboard focus) resolves to somewhere they were
   *not* looking. Asking costs a second; acting on the wrong monitor in a headset costs
   more. The exceptions are listed in §5 and each one is a case where there is no
   monitor to get wrong.

**`active` is a word the user has to say.** `active` / `focused` / `current` /
`selected`, spoken, mean the compositor's own selection and resolve to the literal token
`active`. That is now the *only* path by which a monitor verb emits `active` as a
deliberate target.

---

## 2. What `active` resolves to in the compositor

Read from Hyprland `src/openxr/OpenXRManager.cpp` (round-8 audit, read-only):

```cpp
// OpenXRManager.cpp:3538  COpenXRManager::resolveSelected()
1. m_selectedMonitor      // whatever `hyprctl openxr select <name>` last set — STICKY
2. m_lastHoveredMonitor   // last monitor crossed by the POINTER RAY (not gaze dwell)
3. Desktop::focusState()->monitor()   // …but only if that output has an XR layer
```

So `active` is **not** "the focused monitor", and it is **not** the gaze/dwell candidate.
It is a three-tier fallback dominated by a *sticky* explicit selection that can be minutes
old. This matters for the verbs that take no monitor argument at all:

| shape | commands |
|---|---|
| `<verb> <name>\|active …` — token accepted | `place`, `anchor`, `destroy`, `gazegrab` |
| `<verb> …` — **no** monitor argument, always `resolveSelected()` | `distance`, `move`, `rotate`, `scale`, `center`, `adaptive`, `dock`, `undock`, `roam` |
| defines the selection rather than consuming it | `select <name>\|next\|prev` |

`distance` is in the second group: its grammar is exactly `distance <±m>`. **The only way
to aim a distance move at a particular monitor is to `select` it first**, which is why the
executor's `selectIfNamed` step exists and why an empty `action.target` was fatal.

The daemon's job, therefore, is to resolve a CONCRETE NAME from the ring and pass that —
never `active` — for anything deictic. `active` is reserved for utterances that spoke it.

No compositor change is needed for any of this; nothing here is filed as a gap.

---

## 3. Semantics table — before / after

`GAZE@word` = `resolveDeixis(<deictic word ms>)`, name confirmed live in the snapshot.
"Clarify" = ask, actuate nothing.

| Verb | Spoken form | BEFORE (round 7) | AFTER (round 8) |
|---|---|---|---|
| **MoveDist** | "move **this** monitor closer" | **`active`** — deixis discarded outright, no gaze query at all | **GAZE@word** → `select <name>` + `distance` |
| MoveDist | "move the coding monitor closer" | Semantic → `XR-code` | unchanged |
| MoveDist | "move closer" (no subject) | pointer-hover → else `active` | **GAZE@onset** → else pointer-hover → else `active` |
| MoveDist | "come here" / "bring it closer" | motion, no deixis | unchanged (place-deixis correctly dropped) |
| MoveDist | "move the **active** monitor closer" | Focus verb misfire → Clarify | **`active`**, `ETargetSource::Active` |
| **Pick** | "pick **this** monitor up" | GAZE@word → else hover → else `active` | GAZE@word → else **Clarify** |
| **Center / Dock / Undock / Follow / Anchor** | "…**this** monitor" | GAZE@word → else hover → else `active` | GAZE@word → else **Clarify** |
| Center / Dock / … | bare ("center it", "dock it") | hover → `active` | unchanged (see §5) |
| **Place** | "place it **here**" | `active` + GAZE@word place point | unchanged |
| Place | "put **the coding monitor** here" | `active` + **no place point** — the name suppressed the gaze query | **`XR-code`** + GAZE@word place point |
| **CreateMonitor** | "create a monitor **here**" | GAZE@word, but pose from `word − lead − window` | GAZE@word, pose from `word − lead` |
| CreateMonitor | "create a monitor" | compositor default placement | unchanged (see §5) |
| **MoveWindow** dest | "move Plex **here**" / "…to **this** monitor" | GAZE@word → else Clarify | unchanged — already correct |
| **MoveWorkspace** dest | "move workspace 4 to **this** monitor" | GAZE@word → else Clarify | unchanged — already correct |
| **Fullscreen** | "make **this** window fullscreen" | GAZE@word monitor → else `active` window | unchanged (see §5) |
| **Focus** | "focus the browser" | Semantic window | unchanged |
| Focus | bare "focus" | Clarify | unchanged |
| **Workspace / LaunchApp / HandInput** | — | no monitor target | unchanged |

The three rows in bold-with-a-change are the round-8 fixes; everything else is either
already compliant or a documented exception.

---

## 4. Which timestamp each gaze query uses

Every query in the daemon, and the instant it asks about:

| Call site | Timestamp asked for | Correct? |
|---|---|---|
| `finalizeAction` → `resolveDeixis(raw.deicticWordMs, …)` | `STranscript::words[i].startMs` of the trailing deictic — an absolute `CLOCK_MONOTONIC` ms (`CAsr::transcribe` offsets whisper's 10 ms DTW units by the segment's `bufferStartMs`) | yes |
| `findDeictic` fallback, no per-word timestamps | `onsetMs` for "this"/"that", `endMs` for "here"/"there" | yes — best available; only heuristic ASR paths hit it |
| MoveDist, no subject spoken (**new**) | `STranscript::onsetMs` | by decision — there is no deictic word to time from |
| `hyprctl openxr gaze` with no `at` (i.e. "now") | — | **never used by the intent tier** |

There is no path that queries "now". Both live misfires looked like one and neither was.

### The `matchedMs ≈ requestedMs − 505` observation

Real, reproducible, and **not** a wrong query time. Every live create logged it:

```
"requestedMs":44272461, "matchedMs":44271962, "ageMs":499, "dwellSec":0.000, "agree":5/5
```

`499 ≈ deixis_lead_ms (200) + deixis_window_ms (300)` — i.e. the sample used was the
**oldest** point in the stability window, not the word instant. `resolveDeixis` sampled
`[anchor−window, anchor]` correctly, then picked its representative sample by
*"highest `dwellSec`, first one wins a tie"*. A deixis aimed at passthrough — which
"create a monitor **here**" almost always is, since you look at empty space to put a
monitor there — reports `dwellSec = 0.000` in **every** sample. So the comparison tied
everywhere and the first-pushed sample, the oldest, won every single time.

Net effect: the new monitor was placed using a head pose a **half second stale**, always
in the same direction. On a head that is still turning while it speaks, that is a large,
*systematic* offset — which is what the "it went where I was looking, not where I'm
looking" report actually is.

**Fix:** the representative sample is now the agreeing sample asked for *nearest the
lead-shifted word*, tie-broken by dwell. The vote still spans the whole window (a saccade
still cannot hijack the identity); only the POSE moved. `ageMs` should now read
`≈ deixis_lead_ms` on every live deixis, and that number is a one-line health check on
this whole mechanism.

---

## 5. Exceptions — where a non-gaze answer is deliberate

Each of these is a case where there is no monitor to get wrong, so a fallback cannot
misfire the way `move_dist` did.

- **`Place` subject** ("place **it** here"). "It" is *anaphoric*, not deictic — it means
  the thing you are already carrying, which is exactly what the grab verbs just fed to
  `resolveSelected()`. Deferring to `active` here is the correct reading, not a guess.
  The deixis in the utterance still governs the POSE and is still resolved at word time.
  A subject the user *names* wins over `active`.
- **`Dock … here`** ("dock it here"). Same anaphoric subject; "here" selects the dock
  pose, and the compositor's `dock here` verb consumes it directly.
- **`Fullscreen`** ("make **this** window fullscreen"). The subject is a *window*, and
  with no window named the focused one is both the right reading and reversible — the
  dispatcher is a toggle. The gaze deixis, when it resolves, is still honoured: the plan
  prepends `focusmonitor <gazed>` so "this" means what you are looking at.
- **`CreateMonitor`** with no deixis ("create a monitor"). No placement is emitted at all
  and the compositor's default (in front of you, at `default_distance`) stands. There is
  no existing monitor to disturb.
- **Bare `Center` / `Dock` / `Undock` / `Follow` / `Anchor`** ("center it"). These keep
  the historical pointer-hover → `active` fallback. None has misfired live, and unlike
  `move_dist` they are either self-evidently reversible or already require an explicit
  mode word. Extending the gaze-at-onset treatment of §3 to them is a one-line change
  in `finalizeAction`, deliberately deferred until a live round asks for it.

### Bare verbs: why `move closer` is gaze-first

"Move closer" names no subject. Two readings compete: *the monitor I am looking at*, and
*the monitor the compositor has selected*. We chose gaze-first, because `resolveSelected()`
tier 1 is a **sticky** `m_selectedMonitor` that persists until something else sets it — it
can easily be a monitor the user last touched minutes ago and has since looked away from.
Gaze is at worst as good and usually exactly right. The fallback order is unchanged
beneath it, so nothing that worked before stops working: gaze → pointer-hover → `active`.

The query instant is the utterance ONSET — the moment the user decided to speak, which is
the closest thing a subject-less phrase has to a word timestamp.

---

## 6. Where this lives in the code

| Concern | File |
|---|---|
| Deictic word + its timestamp | `src/Intent.cpp` `findDeictic()` |
| Per-verb parse, place-deixis vs monitor-deixis | `src/Intent.cpp` `CRuleIntent::detect()` |
| The single resolution point for every backend | `src/Intent.cpp` `finalizeAction()` |
| Spoken-`active` recogniser | `src/Intent.cpp` `namesActiveTarget()` |
| Ring query, vote, representative pose, place-point projection | `src/GazeResolver.cpp` `resolveDeixis()` |
| Concrete-name → `select` before an argument-less verb | `src/Executor.cpp` `selectIfNamed()` |
| Regressions | `tests/test_intent.cpp`, `tests/test_gaze.cpp`, `tests/test_executor.cpp` (round-8 blocks) |
