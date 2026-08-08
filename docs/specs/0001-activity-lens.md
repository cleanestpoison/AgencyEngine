# Spec: the Activity lens

_Labels: `ready-for-agent`_
_Related: [ADR-0001 — Proposals are a distinct kind of impulse from topics](../adr/0001-proposals-are-a-distinct-kind-of-impulse.md)_
_Vocabulary: see [CONTEXT.md](../../CONTEXT.md) — **Lens**, **Impulse**, **Topic**, **Proposal**, **Pending impulse**, **Ledger**, **Beat**, **Standing**, **Resolution**._

## Problem Statement

Companions can raise two kinds of thing unprompted: a **serious agenda** (the Aspiration lens) and an
**unsaid standing** between people (the Relationship lens). Both are heavy. Neither can produce the
most ordinary thing a travelling companion does — ask you to do something *with* them. Nobody suggests
a drink, asks for a round of sparring, proposes a game by the fire, says they are restless and want
something to fight, or asks for your company for its own sake.

The result is a party whose only unprompted speech is grievance and agenda. A companion who exclusively
raises serious matters is as badly drawn as one who never raises anything.

There is a second, quieter problem underneath. The **Aspiration lens** does not actually mean what its
name says: it currently owns mundane appetites ("rest, food… a road, a place") and its lead example is
a companion who wants a bed. So the one register that is supposed to carry weight is diluted, and any
new lens covering daily life would collide with it head-on.

## Solution

A third lens — **Activity** — covering daily life: appetites, downtime, and invitation. Drinking,
sparring, a game or a wager, a story by the fire, restlessness for a fight, an outing, closeness where
the **standing** supports it.

Aspiration narrows to what its name promises: goals, a quest being walked past, an order of operations
a companion disagrees with. Its mundane-appetite material moves to Activity. The two lenses stop
competing and both get sharper.

Activity is the first lens that produces **proposals** rather than **topics**, and that distinction is
load-bearing rather than cosmetic:

- A **topic** is met when someone answers it. Agreeing, refusing, or arguing it out all count.
- A **proposal** is not met by agreement. "Yes, let's spar" followed by no sparring is a deferral, and
  the mod's own resolution rules already say deferral is not an answer. The companion keeps carrying it
  and raises it again — which is the behaviour that makes a proposal land at all.

Because proposals also come from a small closed vocabulary — a companion has perhaps ten activity
subjects in her life, against unlimited unique topics — the **ledger** must stop treating both the same
way.

## User Stories

### The player, in game

1. As a player, I want a companion to ask me to share a drink, so that travelling together feels like
   company rather than logistics.
2. As a player, I want a companion to ask me to spar while there is light left, so that downtime has
   something in it besides waiting.
3. As a player, I want a companion to propose a game or a wager by the fire, so that camp is a place
   something happens.
4. As a player, I want a companion to offer to tell me something about themselves unasked, so that I
   learn who they are without exhausting a dialogue menu.
5. As a player, I want a companion to say they are restless and ask whether there is anything nearby
   worth clearing out, so that the desire to fight comes from them and not from my quest log.
6. As a player, I want a companion to suggest an outing that serves no purpose — a market, an evening
   somewhere — so that not everything the party does is a task.
7. As a player, I want a companion to raise closeness when the standing between us supports it, so that
   a relationship progresses through something other than my initiating it.
8. As a player, I do **not** want a companion to propose intimacy or deep familiarity out of nowhere, so
   that the relationship reads as earned rather than scripted.
9. As a player, I want a companion to propose something to *another* companion sometimes, so that the
   party has a life that does not route through me.
10. As a player, I want most ticks to still produce silence, so that companions read as people rather
    than as a queue of prompts.
11. As a player, I want a proposal I agreed to but never did to come back, so that saying yes and
    forgetting has a consequence.
12. As a player, I want a proposal I plainly refused to be dropped, so that refusing is respected and
    does not become nagging.
13. As a player, I want a proposal I actually acted on to stop being raised, so that doing the thing is
    acknowledged.
14. As a player, I want a companion to be able to ask for a drink again after several other beats, so
    that recurring pleasures recur rather than being spent forever on first use.
15. As a player, I do **not** want a companion to propose the same activity two impulses running, so
    that she does not read as fixated.
16. As a player, I want an activity proposal to arrive as an opening rather than a plan, so that I can
    decline without a scene.
17. As a player, I want a companion to not propose a drinking game while someone is bleeding out, so
    that the mod reads the room.
18. As a player, I want the proposal to name what she wants rather than where to go, so that I am never
    sent looking for a place the mod invented.
19. As a player who has played for weeks, I want the serious lenses to keep working exactly as they did
    before I enabled Activity, so that adding a lens does not degrade the ones I already tuned.

### The mod user, in the control panel

20. As a mod user, I want Activity to appear as a row in the lens table with a name, prompt file and
    weight, so that it is tuned exactly like the lenses I already understand.
21. As a mod user, I want to set Activity's weight to 0, so that I can turn it off without uninstalling
    anything.
22. As a mod user, I want a per-lens spoken/quiet counter for Activity, so that I can tell whether it is
    running dry or running hot.
23. As a mod user, I want to declare on each lens row whether it produces proposals, so that a lens I
    write myself gets the right resolution behaviour.
24. As a mod user, I want to set a ledger slot count per lens, so that a lens with a small vocabulary
    does not veto itself into silence.
25. As a mod user, I want a per-lens slot count of 0 to mean "use the global setting", so that I do not
    have to configure every row to change one.
26. As a mod user, I want to rename a lens on the settings page without changing how it behaves, so that
    the label stays a label.
27. As a mod user upgrading from a previous version, I want my existing settings file to load with my
    weights and tuning intact, so that an upgrade is not a reset.
28. As a mod user, I want the startup log line to state the new per-lens fields, so that a log someone
    sends me describes the configuration it was produced under.
29. As a mod user, I want the ledger view to show which lens each slot belongs to, so that I can see why
    a subject is or is not suppressed.
30. As a mod user, I want the log to name every eviction and every suppression, so that a lens gone
    quiet is diagnosable after the fact.

### The maintainer

31. As a maintainer, I want the Activity lens to extend the shared base like the other two, so that the
    output contract cannot drift between lenses.
32. As a maintainer, I want Aspiration and Relationship to be byte-identical in behaviour after this
    change, so that a regression in them is attributable to something else.
33. As a maintainer, I want resolution behaviour keyed off a declared property rather than a lens name,
    so that a user renaming a row cannot silently change semantics.
34. As a maintainer, I want ledger eviction scoped per lens, so that a busy lens cannot release another
    lens's settled subjects.
35. As a maintainer, I want ledger slots persisted from an older version to keep working, so that an
    upgrade does not lose or corrupt what companions have already raised.
36. As a maintainer, I want the cross-lens eviction rule covered by an automated test, so that the one
    failure with no visible symptom cannot come back.
37. As a maintainer, I want the README lens table and the prompt inventory to describe the new lens, so
    that the documented design matches what ships.

## Implementation Decisions

### Lens declaration

- `Lens` gains two fields: a boolean declaring whether the lens produces **proposals** (default false,
  preserving today's behaviour for every existing lens), and an integer per-lens ledger slot count where
  **0 means "inherit the global slot count"**.
- Both are surfaced on the Lenses tab: a checkbox column and a numeric column alongside name, prompt
  file and weight.
- Per repo convention, adding settings fields means touching the settings struct, load, save, the
  one-line summary written to the log, and the UI. All five.
- **Behaviour never branches on `Lens::name`.** The name is a free-text field the user can edit; a
  rename must not change semantics. This is the substance of ADR-0001.

### Defaults shipped

- Activity ships as a third lens row with the proposal flag set and a ledger slot count of 3.
- Weights are rebalanced to **Aspiration 35 / Relationship 25 / Activity 40**, reflecting that Aspiration
  loses its mundane material and Activity has the most available.
- Existing installs whose settings file predates this change load with the proposal flag false and the
  per-lens slot count 0 on every row — i.e. exactly today's behaviour — and their weights untouched.

### Prompts

- Activity is a lens file that extends the shared base and overrides prose blocks only, like the other
  two. No standalone contract.
- The base grows **one new overridable block** carved out of its hard constraints: the line stating that
  a topic must come from aspirations, history, standing or accumulated grievances. Its default text is
  today's text verbatim, so Aspiration and Relationship render identically to before. Activity overrides
  it to source the subject from what the companion would want to *do* with these people, given the hour
  and the standing between them.
- The new block is **top level and unconditional**, per the existing rule that no block sits inside an
  `if` or a `for` and no lens reads a variable the base sets.
- The Aspiration lens is narrowed: its mundane-appetite bullet and its want-a-bed example move to
  Activity; its focus, task and question blocks are retightened around goals and order of operations.
- Activity may raise an appetite for a destination ("is there anything nearby worth clearing out") but
  **must never name a place**. The base's existing no-geography constraint and its rule that recalled
  world knowledge is not evidence both stand unchanged. The party acts on what the impulse says, and
  the downstream dialogue system has the real scene in front of it.
- Intimacy appears as one item among the others, phrased as an invitation and explicitly conditioned on
  the standing section supporting it — never as a first move, never invented, never written past the
  asking. **No third-party mod is named in any tracked file**, which is a separate concern from content
  and remains in force: naming one publishes a load order and asserts a dependency.

### Resolution

- The resolution context gains the proposal flag, carried on the pending-impulse entry from dispatch
  (the entry already carries its lens).
- The resolution prompt branches on it. For a proposal, agreement alone is a **deferral**: resolved is
  true only once the events show the thing happened, or someone plainly refused. This reuses the
  existing "deferral is not an answer" rule rather than introducing a new verdict.
- Unchanged: false remains the recoverable answer and the default when the model cannot tell.

### Ledger

- A ledger slot gains a lens key. **Eviction and veto are scoped to the lens**; rendering of "already
  raised" into the prompt stays combined, so no lens repeats another lens's subject.
- Slots restored from a sidecar written by an older version have no lens key. They are treated as legacy
  and retain today's shared-ring behaviour rather than being migrated or discarded.
- The rationale for the count-based, clock-free eviction is unchanged and still correct for topics. The
  per-lens ring is what makes it also correct for proposals: an Activity ring of 3 gives a natural
  four-proposal rotation, so a drink becomes available again after three other proposals rather than
  being spent for six beats of a vocabulary that only has about ten entries.

### Explicitly not built

- No second Director loop and no second timer. Activity is selected by the existing weighted draw with
  the existing no-repeat rule.
- No trigger conditions or eligibility predicates on lenses. If the Status counters later show Activity
  firing at consistently wrong moments, that is the evidence for building them — not a guess now.

## Testing Decisions

**What makes a good test here:** it exercises external behaviour through a public interface, with
arguments identical to the ones production passes, and asserts an outcome a user could describe. It
does not reach into internals, does not assert on log text, and does not encode eviction order beyond
what the behaviour promises.

**Prior art: none.** This repo has no test target, no CTest wiring and no test framework today.

**The one seam: the ledger API on the pending-impulse module.** Chosen because it is the only part of
this change that (a) is pure state with no game, SkyrimNet or LLM dependency, and (b) can regress two
already-shipped lenses with no visible symptom. Its functions take integers, strings and a cap — there
is nothing to mock, so a test calls exactly what the Director calls.

A test target is added, excluded from the default build preset so a plain plugin build is unaffected.
The module compiles without the game precompiled header, which supplies the logging alias; a small
logging shim stands in.

Cases:

1. Recording proposals against a companion whose other-lens ring is full evicts none of the other
   lens's subjects. **This case fails against today's code** and is the reason the seam exists.
2. A lens ring at its slot count recycles its oldest subject and keeps the rest suppressed.
3. A per-lens slot count of 0 falls back to the global count.
4. A provisional slot confirmed keeps suppressing; withdrawn, it stops.
5. A repeat of a subject already held moves it to newest rather than consuming a second slot.
6. A legacy slot carrying no lens key keeps shared-ring behaviour and is not dropped.

Loose subject matching (case and surrounding punctuation ignored) is covered incidentally by using
differently-cased subjects in the cases above.

**Left to in-game verification, deliberately:** every prompt change, because there is no offline
renderer for the templating system or its decorators; the Director plumbing the per-lens slot count,
which a direct call to the ledger cannot catch; and all judgement — whether the model writes a
consistent subject slug, whether a proposal reads well, whether the lens fires at sensible moments.
The verification surface for those already exists: the per-lens spoken/quiet counters, the history
view's context payload for the most recent dispatch, the ledger view, and log lines that name every
eviction, every suppression and every declining path.

## Out of Scope

- Making activities actually happen. This mod supplies the agenda and never the line; driving sparring,
  drinking or a game is a different mod and belongs to the tier where companions act rather than ask.
- A second loop, a second timer, or per-lens trigger conditions and cooldowns.
- Any environment or affordance context — nearby objects, furniture, weather, indoor/outdoor. Activities
  are social proposals; the party is the affordance, not the scenery.
- Naming destinations, and therefore any quest-log or world-knowledge integration to license them.
- Clock-based ledger expiry. The per-lens ring solves the recurrence problem without adding a clock to a
  structure that deliberately has none.
- Tracked threads that persist and escalate across impulses — that remains the next tier.
- Any change to how impulses are delivered, audienced, or written back.

## Further Notes

- The ordering matters: narrowing Aspiration is not optional polish. Without it, Activity and Aspiration
  compete for the same material and the weighted draw merely splits one lens's output across two names,
  which is worse than not shipping the lens.
- The cross-lens eviction problem is a **regression in already-shipped behaviour**, not a limitation of
  the new feature. Enabling Activity without the per-lens ring would make Aspiration and Relationship
  worse, invisibly, over the course of a long playthrough. It is the single highest-risk item here.
- The resolution branch is what makes proposals worth having. Without it the lens buries its own impulse
  at the exact moment the player says yes and before anything has occurred — the one case that should
  keep the companion carrying it is the one that clears it fastest.
- Expect Activity to be the loudest of the three lenses. That is intended and is why it takes the largest
  weight, but the per-lens counters are the readout: a lens that speaks on most ticks is a nag, and the
  correction is the weight, not the prompt.
