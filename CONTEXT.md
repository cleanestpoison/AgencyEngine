# AgencyEngine

The domain language of a plugin that lets companions **start** things. Everything here is about one
loop: read what has been happening to the party, decide whether anyone has something they would raise
unprompted, and hand them a speaking turn on it.

## Language

### The loop

**Impulse**:
A single decision that one companion has something to raise right now, expressed as a stage direction.
_Avoid_: prompt, generation, event

**Tick**:
One firing of the loop, on the in-game interval. Most ticks produce silence, and that is the design.
_Avoid_: cycle, poll

**Quiet tick**:
A tick that returned no speaker. The common and correct outcome, counted separately per lens.
_Avoid_: failure, miss, empty

**Stage direction**:
The 2–4 sentence, third-person, present-tense text an impulse carries. It names a topic, a why-now,
and an opening. It never contains the companion's words or their conclusion — they write the line.
_Avoid_: narration text, dialogue, script

**Forced turn**:
A tick on which the silence option is removed from the prompt and someone must speak. A percentage
setting, rolled inside the template, suppressed while the party is mid-exchange.
_Avoid_: guaranteed impulse, always-on

### Lenses

**Lens**:
One focused question the loop can ask, shipped as a prompt file that extends the base and overrides
prose blocks only. Selection is weighted and happens in the DLL, never in the template.
_Avoid_: mode, prompt type, category

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
by a decorator. One per companion; a second supersedes rather than stacks. Never an event — events
carry an audience derived from proximity and cannot be made private.
_Avoid_: memory, thought, note

**Carried / Spoken**:
The two states of a pending impulse. *Carried* means she has not said it; *spoken* means the loop gave
her the turn and she did. They are different questions and the resolution check asks each differently.
_Avoid_: pending/delivered, open/closed

**Resolution**:
The periodic check asking whether a pending impulse has been *met*. `false` is the recoverable answer
and the default when unclear; `true` buries the subject.
_Avoid_: completion, closure, expiry

**Ledger**:
Per-companion record of subjects already raised, rendered into the prompt as a closed door. Eviction is
by count, not by clock — a quiet in-game week must not make a settled grievance raisable again.
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
- A **Lens** declares which kind it produces; this is *not* inferred from its name, which users can edit
- An **Impulse** becomes at most one **Pending impulse**, which is **Carried** and may become **Spoken**
- **Resolution** decides a **Pending impulse**, and the question it asks depends on Topic vs Proposal
- A resolved **Pending impulse** confirms a **Ledger** slot; an unresolved one withdraws it
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
- **"Lens name"** was nearly used as an identity for behavioural branching. Resolved: it is a *label*,
  user-editable in the UI. Behaviour branches on declared properties of the lens, never on its name.
