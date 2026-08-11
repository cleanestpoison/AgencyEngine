# Delivery becomes multi-carry, and a coalescing cue replaces the narrated stage direction

Delivery was built for one impulse at a time: the decision came back, and the stage direction went
out as a direct narration that did two jobs at once — granted the companion a speaking turn and
carried the topic she was to raise. ADR 0003 breaks the one-at-a-time assumption. With every lens on
its own clock, one Director pass can produce several carries, across lenses and across companions,
and a delivery that narrates each one either interrupts the party back-to-back or serializes into a
queue whose tail speaks state minutes stale.

So the two jobs come apart. **The stage direction becomes the carried payload**: a pending impulse
is now one per companion *per lens* — a second from the same lens supersedes, as before — and every
impulse a companion carries renders into her own character bio at carry time, newest first, through
the decorator that already exists. Direct narration no longer transports it.

What remains for narration is the announcement. A new carry sets one pending **cue** per companion:
a vague direct narration — she seems to have something on her mind — that grants the speaking turn
and names no topic, because the bio supplies the material. That ordering is load-bearing: the store
write must land before the narration goes out, or the turn arrives before the agenda does. Cues
coalesce — any number of fresh carries, one pending cue, one sentence that stays true whether she is
carrying one impulse or three.

The cue waits for a conversational lull through the existing held-delivery machinery (the quiet
reading, `maxDeferSeconds`). If conversation happened between the cue being set and the lull, the
resolution pass runs first: anything the exchange already met is cleared and its ledger slot
confirmed, and the cue fires only if an impulse survives — the gate that stops her announcing a
subject the party just dealt with. If the party stayed silent, the check is skipped and the cue
fires: the event tail shows the silence without spending an LLM call. And `maxDeferSeconds`
expiring simply drops the cue. The impulse is already visible in her bio, so drift — the topic
colouring whatever she says next — is the built-in fallback, not a degrade path.

## Considered Options

- **Narrate each carry, as today, and let them queue.** Rejected: several clocks expiring together
  is now the normal case, not a collision, and a queue of stage directions is either back-to-back
  interruptions or narrations delivered long after the why-now they were written against. The
  narrated text would also duplicate the payload the bio now renders — two copies of one topic,
  free to drift apart.
- **Put the topic in the cue.** Rejected: a cue that names a subject is false the moment a second
  carry coalesces into it, and the bio already delivers the stage direction verbatim. The vague
  sentence is the only one that stays true under coalescing — and it is also what keeps the cue
  cheap enough to drop.
- **Keep `degradeToPersistentEvent` as the expiry fallback.** Retired, with its setting: it wrote an
  *unspoken* impulse through the event path, and SkyrimNet stamps a scene-wide audience onto every
  event at creation — the full finding is recorded in `PapyrusBridge.h`. The "recorded, not spoken"
  degrade was therefore public to every bystander in the scene. Bio drift now covers the same case
  privately and without a write.
- **Persist cue flags in the sidecar.** Rejected: a flag lost to a reload costs one announcement,
  and the bio drifts — harmless. Carried impulses persist where they already do; a cue is not worth
  new save state.
- **Run the resolution pass before every cue fire.** Rejected: when the party stayed silent between
  cue and lull, nothing could have been met, and the tail proves it for free. The pass is an LLM
  call and runs only when there was conversation to judge.

## Consequences

- **`cues` toggle**, bool, default on. Off is pure bio drift — no narration at all, topics surface
  only as they colour her lines. A standard field add (`Settings.h`, `Load`, `Save`, `Summary`, UI),
  and under ADR 0002 a shipped default that reaches every install that hasn't moved it.
- The **`delivery` enum retires** along with `degradeToPersistentEvent` — narration versus
  persistent event is no longer a choice anyone makes, because the payload always travels by bio
  and the only narration left is the cue, which the toggle owns. Both become obsolete keys under
  ADR 0002's loader: ignored on load, logged once. They are already on the consolidated
  obsolete-key list in ADR 0003.
- Cue state is ephemeral by design: no new save state, no new sidecar fields. What persists is
  exactly what persisted before — the carried impulses.
- The resolution check keeps its ADR 0001 semantics (topics met by an answer, proposals only by the
  thing happening or a plain refusal); what changes is that the resolve gate can now invoke it on
  the way to a cue, and a met impulse confirms its ledger slot there rather than waiting for the
  periodic check.
- The implementation must touch two documents this ADR deliberately does not edit: the README's
  delivery description (the loop's steps 3–4, the delivery-mode line under Settings, and every
  persistent-event mention), and the CONTEXT.md glossary — **cue** enters the language, and so does **drift**, the
  fallback it leaves behind (a carried impulse colouring what she says next through her bio, with
  no turn granted); **pending impulse** rewords from one per companion to one per companion per
  lens; **stage direction** updates its delivery clause to carried payload, not narrated text.

## Open validations

Recorded as validations, not decisions — both need a play pass to settle.

- **Multi-carry rendering is unobserved in play.** One carried topic surfaces cleanly; whether three
  stacked impulses surface one at a time or mash into a single overloaded turn is unknown. The
  mitigation lives in the decorator — newest first, framed as the foremost thing on her mind — and
  may need tuning once observed.
- **The voiced-but-not-met case.** A topic she raised that nobody answered survives the resolve
  gate, so a later cue can prompt her back to it. Accepted as persistence rather than malfunction —
  an unanswered subject *should* survive — but worth watching for how it reads at the table.
