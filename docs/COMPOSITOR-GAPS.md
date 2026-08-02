# Compositor gaps — verbs `hypxrvoice` needs from HypXRland

WP-V4 built the executor against the `hyprctl openxr` / `xrmonitor` verbs that ship
**today** (`docs/openxr/05-configuration.md` §4–5). Two spoken interactions cannot be
expressed *precisely* with the current verb set, because a voice command executes
**1–3 s after** the word that scoped it — by then the head/gaze has moved on, so the
"current" gaze candidate is the wrong monitor and the "current" pose is the wrong drop
point. Each gap is listed with the **exact proposed verb**, its semantics, the
**approximation the executor uses today** (behind a capability flag, default off), and
the acceptance test the compositor side should satisfy.

None of this blocks v1: the approximations are shipped and tested. The proposed verbs
turn "good enough" into "pixel/pose exact". All are `HAVE_OPENXR`-guarded, read the
same selection/anchor machinery that exists, and ride the hypxrland branch.

---

## GAP 1 — targeted gaze grab: `gazegrab <name>`

**Why.** The locked interaction model's primary gesture is "pick **this** monitor up".
`hypxrvoice` resolves "this" to a concrete monitor by querying the gaze ring **at the
timestamp of the word "this"** (`hyprctl -j openxr gaze at <ms>`), so it knows the
*correct* target even though execution is seconds later. But the only grab verb today —

```
xrmonitor gazegrab            # toggles a carry on the CURRENT dwell candidate
```

— grabs whatever the user happens to be looking at **at execution time**, not the
monitor they named. There is no way to start a carry on a *named* monitor.

**Proposed verb.**

```
xrmonitor gazegrab <name>     # begin a gaze carry on the NAMED monitor (same carry
                              # machine as the argument-less form), regardless of the
                              # current dwell candidate. Errors cleanly if <name> is
                              # not a live XR monitor or is already hand/controller-grabbed.
hyprctl openxr gazegrab <name>
```

Semantics: identical to today's `gazegrab` (monitor follows the gaze ray until
released via `gazerelease`), only the *initial* target is `<name>` instead of the dwell
candidate. This is a one-line change to the selection step inside the existing
gaze-carry entry point.

**Executor today (approximation, `executor.targeted_grab=false`).** `select <name>`
then `anchor <name> head`: the named monitor is re-anchored to the head frame and so
follows the head immediately — the same "picked up, now moving with me" feel, and it is
**targeted + deterministic** (no dependence on live dwell). It differs from a true gaze
carry in that it tracks head *translation+rotation* (HUD-like) rather than the gaze ray,
and it changes the anchor mode rather than entering the transient carry state.

**Capability flag.** `executor.targeted_grab=true` → emit `openxr gazegrab <name>`.

**Accept.** With the user looking at monitor B, `gazegrab A` starts a carry on **A**
(not B); `-j openxr` shows `A.grabbed=true`.

---

## GAP 2 — place at a resolved pose: `place <name> at <x,y,z>`

**Why.** "place it **here**" scopes the drop point to where the user was looking when
they said "here". `hypxrvoice` resolves that to a `LOCAL_FLOOR` point from the gaze ring
at the word's timestamp. The only "drop" verb today —

```
xrmonitor anchor <name> local # re-anchor without moving the quad (freeze in place)
```

— freezes the monitor at **its current pose**, which is "drop it where it already is",
not "drop it where I'm pointing". For a body/head-carried monitor these differ.

**Proposed verb.**

```
xrmonitor place <name> at <x>,<y>,<z>   # re-anchor <name> to `local`, MOVING the quad
                                        # so its center sits at the given LOCAL_FLOOR
                                        # point (meters), facing the user. Orientation
                                        # optional; default = face the headset.
hyprctl openxr place <name> at 0.5,1.4,-1.2
```

Semantics: like `anchor local`, but sets the world position to the supplied point
first. The point is exactly what `hyprctl -j openxr gaze` already returns
(`head.pos`/hit-point in `LOCAL_FLOOR`), so the coordinate spaces line up with no
conversion.

**Executor today (approximation, `executor.place_at_pose=false`).** `anchor <name>
local` — freeze in place. The pose the daemon resolved is still attached to the emitted
action (for the feedback HUD), just not actuated as a move.

**Capability flag.** `executor.place_at_pose=true` → emit `openxr place <name> at x,y,z`
using the resolved gaze point (numbers formatted by the executor, never transcript
text).

**Accept.** `place XR-chat at 0.5,1.4,-1.2` leaves `XR-chat` world-locked with its
center at that point; `-j openxr` shows `anchor.mode=local` and the new `pose.pos`.

---

## GAP 3 (CLOSED — verified round 8) — name the resolved selection

**Both halves ship.** `COpenXRManager::selectedName()`
(Hyprland `src/openxr/OpenXRManager.cpp:3556`) exposes the resolved name and backs the
`"selected"` field of `openxr status` (`src/openxr/XRIpc.cpp:65,122,167`); `cmdSelect`
fires the `xrmonitorselect` socket2 event. Nothing further is needed here. The original
request is kept below for provenance.

Already proposed in `research/VOICE-CONTROL.md` §8a (WP-T2/T3); restated here because the
executor's `active` path benefits directly.

- **`selected` field in `openxr status` JSON** — expose which monitor `active` resolves
  to (explicit `select` > last hovered > focused-if-XR). Lets `hypxrvoice` name the
  target in feedback ("dropping **XR-chat** — ok?") without replicating the resolution
  order. Pure status addition.
- **socket2 event `xrmonitorselect>><name>`** — `select` fires no event today, so the
  daemon must poll after every selection. An event keeps its dialogue state push-driven.

**Executor today.** Emits `active` and lets the compositor resolve it; feedback names
the target only when the daemon resolved a concrete name itself (semantic/deixis).

---

## GAP 4 (CLOSED — verified round 8) — the gaze reply carries no ray/monitor INTERSECTION point

**Shipped.** Hyprland `68a6eb20` ("openxr: report the gaze ray/quad hit point in
`hyprctl openxr gaze`") added `gaze.hitPoint` + `gaze.hitDistM`, emitted whenever the ray
actually crosses a quad (`src/openxr/XRIpc.cpp` `openxrGaze()`). The daemon needed no
change to consume it — `SGazeSample::parse` has read `gaze.hitPoint` opportunistically
since round 5, and `SGazeResolution::placeFromHit` records which path was taken. The
projection below remains in use for passthrough deixis, which is the common case for
"here", and the minimum-distance clamp still applies to both paths. Original text kept
for provenance.



`hyprctl -j openxr gaze [at <ms>]` returns `head.pos`, `head.quat`, `head.forward`, and a
gaze CANDIDATE (`gaze.monitorId` / `gaze.name` / `gaze.selected` / `gaze.dwellSec`) — but
not the point where the ray actually met a quad. The compositor computes that
intersection every frame to decide `gaze.monitorId`; it simply is not reported.

The daemon therefore projects: `head.pos + normalize(head.forward) * intent.place_distance_m`
(default 1.3 m), floored at `intent.place_min_distance_m`. That is correct for
passthrough deixis ("here", pointing at nothing), but for "put it where I'm looking"
while gazing at an existing monitor it lands at a fixed distance rather than ON the
surface being looked at.

**Wanted (pure status addition, no new verb).** Two optional fields inside the existing
`gaze` object, present only when `selected` is true:

```json
"gaze": { "monitorId": 3, "name": "XR-code", "selected": true, "dwellSec": 0.42,
          "hitPoint": [0.21, 1.33, -2.14], "hitDistM": 2.14 }
```

`hitPoint` in `LOCAL_FLOOR` meters — the same space `place` consumes, so it needs no
conversion. `hitDistM` is redundant but free and makes "put it as far away as that one"
trivial.

**Daemon today.** `SGazeSample::parse()` already reads `gaze.hitPoint` (and `gaze.point`)
opportunistically and prefers it over the projection when present, so shipping the field
alone switches the daemon over with no release on this side. `SGazeResolution.placeFromHit`
records which path was taken.

**Note for whoever implements it.** The clamp stays regardless: the daemon pushes ANY
candidate point — projected or compositor-reported — out to at least
`place_min_distance_m` from the head. A monitor placed inside the wearer's head is the
one outcome that must be structurally impossible.

---

## GAP 5 (open, low priority) — aiming a relative move needs a STICKY side effect

**Why.** `distance`, `move`, `rotate`, `scale`, `center`, `adaptive`, `dock`, `undock` and
`roam` take **no monitor argument at all** (`cmdDistance`'s grammar is literally
`distance <±m>` — Hyprland `src/openxr/OpenXRManager.cpp:4053`). They act on whatever
`resolveSelected()` returns:

```
1. m_selectedMonitor     — what `openxr select <name>` last set. STICKY, no expiry.
2. m_lastHoveredMonitor  — last monitor crossed by the pointer ray.
3. focused output, if it has an XR layer.
```

So the only way for `hypxrvoice` to aim "move **this** monitor closer" at the monitor the
user was actually looking at is to emit `openxr select <name>` first. That works — it is
what the executor does, and round 8's fix makes it happen — but it **mutates global
compositor state as a side effect of a one-shot voice command**. After the utterance,
`active` means something different for every later keybind, script and dwell interaction,
and nothing reverts it. A user who says "move this monitor closer" while looking at
XR-web has silently re-pointed their `openxr center` keybind at XR-web too.

**Proposed.** An optional leading target token on the relative verbs, resolved for that
invocation only and leaving `m_selectedMonitor` untouched:

```
hyprctl openxr distance [<name>|active] <±m>
hyprctl openxr center   [<name>|active]
hyprctl openxr dock     [<name>|active] [here]
…
```

Back-compatible: the existing one-token forms are unambiguous (`distance -0.25` — a
signed number is not a monitor name), and omitting the token keeps today's
`resolveSelected()` behaviour verbatim. `place <name>|active at …` and
`anchor <name>|active <mode>` already have exactly this shape, so it is a consistency fix
as much as a feature.

**Acceptance test.** `openxr select XR-code`, then `openxr distance XR-web -0.25`, then
`openxr status`: XR-web moved, and `selected` still reads `XR-code`.

**Executor today.** Emits `select <name>` + the argument-less verb, and accepts the sticky
side effect. This is *correct* — the right monitor moves — so it does not block anything;
it is filed because the side effect is invisible to the user and surprising later. See
`docs/DEIXIS-SEMANTICS.md` §2.

---

## Verbs used as-is (no gap)

`select`, `anchor <name> <mode>`, `distance <±m>`, `center`, `adaptive on|off`,
`dock [here]`, `undock`, `roam head|body`, `handinput on|off|auto|toggle`,
`create <name> <WxH>[@Hz]` (runtime monitor creation for "create a monitor here" —
runtime-created monitors are never touched by config reconciliation), and
`dispatch exec -- <cmd>` (allowlisted launch) are used verbatim. Hyprland-side:
`dispatch moveworkspacetomonitor <N> <name>`. Read side:
`monitors -j`, `clients -j`, `-j openxr status`, and `-j openxr gaze at <ms>` — all
read-only, all shipping.

**Caveat on the argument-less verbs.** `distance`, `center`, `dock`, `undock`, `roam`,
`adaptive` are used verbatim but must be preceded by `select <name>` to be aimed; see
GAP 5 for the sticky side effect that entails.

**`create` is accepted before the output exists.** `openxr create` returns as soon as the
compositor takes the request; the monitor is registered asynchronously, so a `place
<newname> …` issued in the same plan can arrive before that name resolves. Observed live:
`create XR-2 2560x1440@60` + `place XR-2 at …` failed at 21:51 ("step exited 1 — stopping
plan") and the identical pair succeeded at 22:07. This is fixed daemon-side rather than
counted as a gap: the dependent step declares `SExecStep::waitForMonitor` and `runPlan`
polls `monitors -j` for the name (`executor.wait_monitor_ms`, default 2000, every 100 ms)
before dispatching, refusing the step if it never appears. A synchronous `create` — or a
create reply that lands only once the output is registered — would let the wait go away.
