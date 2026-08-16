# AgencyEngine

The domain language of a plugin that lets companions **start** things. Everything here is about one
loop: read what has been happening to the party, decide whether anyone has something they would raise
unprompted, and hand them a speaking turn on it.

## Language

### The loop

**Impulse**:
A single decision that one companion has something to raise right now, expressed as a stage direction.
_Avoid_: prompt, generation, event

**Ask**:
One call to one lens. Most asks produce silence, and that is the design. It is the unit everything
about cadence and cost is measured in — a lens is asked, not the loop.
_Avoid_: tick, cycle, poll, firing

**Quiet ask**:
An ask that returned no speaker. The common and correct outcome, counted separately per lens.
_Avoid_: failure, miss, empty

**Pass**:
One turn of the Director's own loop, once a second. A pass checks every lens's clock and may make no
LLM calls at all — that is the ordinary case, and it is what a pass is *for*. Never a unit of cadence.
_Avoid_: tick, iteration

**Interval**:
How long a lens waits between asks, in game time. Both the chattiness knob and the cost knob: a quiet
ask is still an LLM call, and there is no tick behind it throttling anything.
_Avoid_: frequency, rate, cadence (which names the pair)

**Cooldown**:
The extra silence a lens takes once its ask has landed as a carried impulse — so a carry costs
interval + cooldown. Keyed on carry, never on her speaking it. This is what makes the anti-nag
structural: a lens cannot re-raise inside its cooldown because it is never asked. Belongs to lens
cadence only; the **ledger** evicts by count and has no clock.
_Avoid_: timeout, backoff, suppression window

**Stage direction**:
The 2–4 sentence, third-person, present-tense text an impulse carries. It names a topic, a why-now,
and an opening. It never contains the companion's words or their conclusion — they write the line.
It is the **carried payload**, rendered into her own bio; it is not narrated at her, and no narration
ever contains it.
_Avoid_: narration text, dialogue, script

**Cue**:
The vague direct narration that announces a fresh carry — she has something on her mind — granting the
speaking turn and naming no subject, because the bio supplies the material. One per companion,
coalescing across however many carries; it waits for the **settle**, not merely for a lull, and
expiring simply drops it.
_Avoid_: announcement, nudge, prompt

**Settle**:
How long since the last conversational turn before the exchange counts as *over* rather than paused.
A second, longer threshold on the same clock the quiet check reads, and the two ask different
questions: quiet says nobody is mid-utterance, the settle says the conversation has ended. Twenty-five
seconds of silence is an ordinary beat in a group chat, and a cue fired into one reads as a companion
changing the subject. A cue waits for both, and its expiry clock stops while it waits.
_Avoid_: cooldown (which is lens cadence), timeout, grace period

**The floor**:
The speaking turn a cue grants, for the half-minute it lasts. The one thing that distinguishes
*carrying* a subject from *looking for a way in*: with the floor she is told to raise it, and without
it the subject only colours what she is already saying. Recorded as a grant rather than read off the
cue, which is erased before the prompt renders.
_Avoid_: turn (ambiguous with a conversational turn), permission, trigger

**Drift**:
A carried impulse colouring what she says next through her bio, with no turn granted. What happens with
cues switched off, and what a dropped cue falls back to. The built-in fallback, not a degrade path.
_Avoid_: leakage, bleed, passive delivery

**Forced turn**:
An ask on which the silence option is removed from the prompt and someone must speak. A percentage
setting, rolled inside the template per ask, suppressed while the party is mid-exchange.
_Avoid_: guaranteed impulse, always-on

### Lenses

**Lens**:
One focused question the loop can ask, shipped as a prompt file that extends the base and overrides
prose blocks only. Each lens runs on its own **interval** and **cooldown**; there is no selection
between them, because asking two questions is not worse than asking one.
_Avoid_: mode, prompt type, category

**Lens roster**:
Which lenses exist. It is *content*, shipped in the build alongside the prompt files it names, not
configuration — a user switches a lens on or off, moves its **interval** and **cooldown**, sets its
ring size, and nothing else about it. This is what lets a later version add or fix a lens, or retune a
cadence, on an install that already has a settings file.
_Avoid_: lens list, configured lenses

**Lens id**:
The stable key a config override is stored under (`activity`, `aspiration`). Never displayed and never
edited, so a lens can be renamed by a release without resetting anyone's cadence. Distinct from the
**ledger ring**, which keys on the lens's *name* because it is per-character saved state.
_Avoid_: lens key, lens slug

**Aspiration lens**:
The serious agenda — goals, a quest being walked past, an order of operations she disagrees with,
what she is trying to accomplish. Explicitly *not* mundane appetites.
_Avoid_: goals lens, wants lens

**Relationship lens**:
Standing between two people — a settled view one companion carries about another, or about the player,
that they have not said.
_Avoid_: social lens, feelings lens

**Activity lens**:
Daily life — appetites, downtime, invitation. Drinking, sparring, a game, a story by the fire,
restlessness for a fight, an outing, closeness. The only lens that produces **proposals**.
_Avoid_: fun lens, leisure lens, social lens

### Kinds of impulse

**Topic**:
An impulse whose payoff is the talking. Produced by the Aspiration and Relationship lenses. Someone
answering it — agreeing, refusing, arguing it out — resolves it.
_Avoid_: subject, matter

**Proposal**:
An impulse that asks the party to *do* something together, where the talking is not the payoff.
Produced by the Activity lens. Agreement does **not** resolve it; only the thing happening, or a plain
refusal, does. "Yes" that leads nowhere is a deferral.
_Avoid_: suggestion, request, activity

**Joint undertaking**:
The test that separates a proposal from an aspiration — strip the other person out and see whether it
survives. A bed survives being alone; a drink together does not.
_Avoid_: shared goal, group activity

### What persists

**Pending impulse**:
The recorded impulse a companion is carrying, held in the DLL and rendered into her own character bio
by a decorator. **One per companion per lens** — a second from the same lens supersedes rather than
stacks, but another lens's is a different question and coexists with it. Every one she carries renders,
newest first. Never an event — events carry an audience derived from proximity and cannot be made
private.
_Avoid_: memory, thought, note

**Carried / Spoken**:
The two states of a pending impulse. *Carried* means she has not said it. *Spoken* meant the loop gave
her the turn on that particular subject and she took it — which the loop can no longer observe, because
a **cue** grants a turn without naming which of the things she is carrying she will raise. Nothing
enters the spoken state now; entries restored from before the cue can be in it, and the resolution
check still asks them their own question. The **resolution** check is what decides a carried impulse.
_Avoid_: pending/delivered, open/closed

**Resolution**:
The periodic check asking whether a pending impulse has been *met*. `false` is the recoverable answer
and the default when unclear; `true` buries the subject.
_Avoid_: completion, closure, expiry

**Ledger**:
Per-companion record of subjects already taken up, rendered into the prompt as a closed door. A slot is
written at **carry** — the loop cannot see which subject she voiced, so carry is the event it owns —
and stays provisional until **resolution** confirms or withdraws it. Eviction is by count, not by
clock: a quiet in-game week must not make a settled grievance raisable again.
_Avoid_: history, cooldown, blacklist

**Beat**:
One ledger slot's worth of impulse. "Suppressed for six beats" means six other subjects must come and
go before this one returns, however long that takes in game time.
_Avoid_: turn, cycle

**Standing**:
Where one companion sits with another, or with the player. Sourced externally, undated and
steady-state — good for *what* is between two people, useless for *why today*. Timing must always come
from stamped thoughts and events.
_Avoid_: relationship score, rapport, affinity

## Relationships

- A **Lens** produces **Impulses**, each of which is either a **Topic** or a **Proposal**
- A **Lens** declares which kind it produces; this is *not* inferred from its name, which is a label
- Each **Lens** is asked on its own **Interval**; a **Carried** impulse costs it a **Cooldown** as well
- The **Lens roster** comes from the build; the config holds the switch, the cadence and the ring size,
  by **Lens id**
- An **Impulse** becomes at most one **Pending impulse**, which is **Carried** and may become **Spoken**
- A companion holds at most one **Pending impulse** per **Lens**, and may hold one from each at once
- **Resolution** decides one **Pending impulse** at a time, and the question it asks depends on Topic vs
  Proposal
- A **Carried** impulse takes a provisional **Ledger** slot at once; **Resolution** confirms it, and
  every other way it can die withdraws it
- Each **Lens** evicts only within its own **Ledger** ring, so lenses cannot bury each other's subjects
- **Standing** supplies the subject for the **Relationship lens** and the licence for the **Activity lens**

## Example dialogue

> **Dev:** She asked him to spar, he said yes, and the resolution check buried it. Bug?
> **Domain expert:** Bug. Sparring is a **Proposal**, not a **Topic**. Agreement resolves a topic —
> he answered her, that's the whole payoff. A proposal isn't met until it *happened* or he plainly
> refused. "Sure" is a deferral, and deferral is not an answer.
>
> **Dev:** So how does the check know which it was?
> **Domain expert:** The **Lens** declares it. Not by name — someone renames "Activity" to "Downtime"
> on the settings page and the branch silently stops matching.
>
> **Dev:** And if they spar every night, doesn't the **Ledger** just fill up with "sparring"?
> **Domain expert:** That's why each lens gets its own ring. Activities come from a closed vocabulary —
> drink, spar, a game, a story — maybe ten subjects she'll ever have. Six shared slots would hold her
> entire repertoire and veto her into silence, *and* evict "her father" to do it.

## Flagged ambiguities

- **"Aspiration"** was used to mean *the serious agenda* in conversation, but the shipped lens also
  owned mundane appetites — rest, food, a bed — and led with a want-a-bed example. Resolved: Aspiration
  narrows to the serious agenda; appetites and downtime move to the **Activity lens**.
- **"Activity"** was initially read as environmental affordance (what furniture is nearby). Resolved:
  activities are *social proposals* — the party is the affordance, not the scenery. Destinations may be
  raised as appetite ("is there anything nearby worth clearing out") but never named, because the party
  acts on what the impulse says and recalled world knowledge is not evidence.
- **"Lens name"** was nearly used as an identity for behavioural branching. Resolved: it is a *label*.
  Behaviour branches on declared properties of the lens, never on its name. A shipped lens's name now
  comes from the build rather than being typed on the settings page, but the rule stands — a lens
  someone wrote themselves still carries a name they chose, and a release is still free to rename one.
- **"Lens" as a config entity** was the original design and is now wrong. Resolved: the **lens roster**
  is content and the config holds *overrides* against it, keyed by **lens id**. A config that could
  contradict the build about a lens's prompt file could only ever be wrong about it.
- **"Weight"** meant a lens's share of a draw that no longer happens, and **"tick"** meant both the
  impulse cadence and the Director's own loop. Resolved by ADR 0003: cadence is per lens (**interval**
  and **cooldown**), the unit is the **ask**, and the loop's iteration is a **pass**. Weight survives
  in one place only — a stored `weight: 0` still reads as "never ask this", because that is how an
  install switched off a lens whose prompt needs a mod it does not have.
