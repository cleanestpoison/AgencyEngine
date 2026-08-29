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

**Lens context**:
The evidence assembled for a lens to judge during an ask: companion profiles, **standing**, stamped thoughts and
events, and memories. External relationship systems may enrich this context without owning the resulting impulse.
Their state is normalized into domain facts rather than copied as dialogue instructions or exposed as raw scores.
Lens context is distinct from character-bio guidance that shapes how a companion speaks after they have the floor.
_Avoid_: prompt injection, dialogue instructions, bio, raw integration state

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

**Curiosity lens**:
A genuine unanswered question a present companion raises about the player because understanding them is
the payoff. The player is always the target. It carries one exact information gap, never a questionnaire,
request, agenda decision, or settled grievance.
_Avoid_: questions lens, backstory lens, interview lens

### Kinds of impulse

**Topic**:
An impulse whose payoff is the talking. Produced by the Aspiration, Relationship, and Curiosity lenses.
Someone answering it — agreeing, refusing, arguing it out — resolves it.
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

**Untouched / Raised-unmet / Met**:
The monotonic lifecycle of a pending impulse. *Untouched* means carried but not observed aloud.
*Raised-unmet* means introduced but unanswered, deferred, or—if a proposal—not yet done. *Met* means
answered, refused, made moot, or, for a proposal, done; met entries retire. A raised-unmet entry remains
open but is never presented as new again.
_Avoid_: carried/spoken, pending/delivered, open/closed

**Entry ID**:
The persisted, save-local identity of one pending impulse. Cues, floor grants, UI actions, evidence
watermarks, memory ownership, and resolver verdicts name this ID so stale work cannot mutate a
replacement occupying the same companion/lens slot.
_Avoid_: row, slot identity, FormID-plus-lens

**Resolution checkpoint**:
An explicitly paid semantic batch due for each open entry after 30 newer accepted SkyrimNet events.
Callbacks provide the low-latency path; one bounded recent-tail poll every 15 active real seconds
recovers missed callbacks through the same source-ID deduplication, after a non-counting load
baseline. Every independently due entry shares one cross-follower request, globally limited to one
automatic batch every four real minutes. Activity advances the checkpoint; silence is irrelevant.
Manual checks bypass both gates. The same evidence cannot pay twice because each entry persists its
last-attempted sequence.
_Avoid_: conversation-settle gate, game-time resolver timer, relevant-event cap, private database reader

**Personal memory / Party memory**:
Personal exact memory suppresses a companion's repeated subject across all lenses while retaining
per-lens insertion and eviction. Party-heard exact memory suppresses cross-follower echoes for seven
game days. Both retain their origin entry ID; provisional personal memory withdraws if an untouched
entry retires, while a raise confirms both layers.
_Avoid_: global blacklist, cooldown

**Evidence sequence**:
A persisted per-save monotonic number assigned after a raw SkyrimNet event leaves its callback.
`ActiveSaveToken { saveId, generation }` additionally binds raw events, batches, and results to one
loaded session; generation changes on every load, including A→B→A.
_Avoid_: event timestamp as identity

**Beat**:
One ledger slot's worth of impulse. "Suppressed for six beats" means six other subjects must come and
go before this one returns, however long that takes in game time.
_Avoid_: turn, cycle

**Standing**:
Where one companion sits with another, or with the player. Sourced externally, undated and
steady-state — good for *what* is between two people, useless for *why today*. For player standing, enrollment in a
dedicated bond record makes that record authoritative even when some facets remain unknown; general rapport is the
fallback only when no such enrollment exists, never a field-by-field supplement. Companion-to-companion standing
remains independent. Timing must always come from stamped thoughts and events.
A progression score alone is not a dedicated bond record: without bond kind, stance and boundaries it cannot
supersede a complete source of player standing.
An unknown facet stays silent: absence of a preference, limit, reason or established orientation is not a neutral
answer and is never filled with a default character fact.
_Avoid_: relationship score, rapport, affinity

## Relationships

- A **Lens** produces **Impulses**, each of which is either a **Topic** or a **Proposal**
- A **Lens** declares which kind it produces; this is *not* inferred from its name, which is a label
- Each **Lens** is asked on its own **Interval**; a **Carried** impulse costs it a **Cooldown** as well
- The **Lens roster** comes from the build; the config holds the switch, the cadence and the ring size,
  by **Lens id**
- An **Impulse** becomes at most one **Pending impulse**, identified by an **Entry ID**, and progresses
  **Untouched** → **Raised-unmet** → **Met**
- A companion holds at most one **Pending impulse** per **Lens**, and may hold one from each at once
- A cue's floor grant owns one exact **Entry ID**; its speaker's opening line raises that entry for zero
  calls, and any later direct speaker/target exchange can make paid **Resolution** eligible without
  literal topic overlap
- **Resolution** batches the independently eligible entry plus other open entries for its participating
  companion against shared ordered evidence; Topic vs Proposal controls what counts as **Met**
- An **Untouched** impulse owns provisional **Personal memory**; raising confirms personal and
  **Party memory**, while untouched retirement withdraws only its provisional record
- Each **Lens** evicts only within its own personal-memory ring, so lenses cannot bury each other's subjects
- Every lens receives the same **Lens context**. Each lens may use shared **Standing** only within its own subject
  boundary; static standing can shape disposition, licence and depth, but never supplies *why today*
- Bond kind and bond depth are independent. Depth remains a six-stage qualitative progression for both platonic
  and romantic bonds; implementation tier names and numbers are not domain language
- Decision-relevant **Standing** includes bond kind and depth, the other person's stated stance, present
  availability, established preferences, and fixed boundaries. Form of address belongs to character-bio guidance;
  progress toward a scoring threshold is neither standing nor timing
- A preference may supply a subject only where the lens owns that question and stamped evidence shows the
  preferred or disliked act occurring. Without that occurrence it is static **Standing** and produces silence
- Preferences are one companion's personal leanings, never global morality or rules they recite to the player
- A reaction surfaced by another relationship system does not automatically spend an AgencyEngine subject.
  The lens judges the full recent exchange under its ordinary repetition rules; there is no cross-system veto
- External **Standing** is read-only context. Its owning relationship system alone asks and records questions that
  change bond kind, consent stance or progression; an AgencyEngine impulse may not impersonate that transition
- A declined romantic stance is not reopened or hinted around. An unanswered stance grants no claim or assumed
  mutuality; until the owning system resolves it, AgencyEngine remains within the established non-transition state
- Romantic commitment and physical availability are separate axes. Present physical availability may license, but
  never compel, an **Activity** proposal without romance; physical unavailability is a hard prohibition
- A permanent physical boundary and present unavailability are different facts. Waiting may change only the latter
- A fixed personal limit overrides every licence supplied by bond, stance, romantic availability or physical availability
- **Standing** supplies the subject for the **Relationship lens** and the licence for the **Activity lens**
- The player's summary and appearance join the companion context for the **Curiosity lens**; the subject
  remains one genuine unknown rather than an assumption about the player's identity or history

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
