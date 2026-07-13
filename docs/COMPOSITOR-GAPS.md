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

## GAP 3 (nice-to-have) — name the resolved selection

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

## Verbs used as-is (no gap)

`select`, `anchor <name> <mode>`, `distance <±m>`, `center`, `adaptive on|off`,
`dock [here]`, `undock`, `roam head|body`, `handinput on|off|auto|toggle`, and
`dispatch exec -- <cmd>` (allowlisted launch) are used verbatim. Read side:
`monitors -j`, `clients -j`, `-j openxr status`, and `-j openxr gaze at <ms>` — all
read-only, all shipping.
