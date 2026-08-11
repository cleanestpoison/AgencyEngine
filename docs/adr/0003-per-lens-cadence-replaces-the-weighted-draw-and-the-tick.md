# Per-lens cadence replaces the weighted draw, and the tick retires with it

Selection used to be a two-dial machine: a shared tick fired every `intervalGameMinutes`, and a
weighted draw picked one lens per tick (skipping the one that fired last). Both dials answer the wrong
question. The tick is one cost-and-chattiness knob shared across lenses whose natural rhythms differ by
an order of magnitude — Relationship's material accumulates over in-game days while Aspiration's turns
over in hours — and the draw makes lenses *compete* for a turn when there is nothing to compete over:
asking two questions is not worse than asking one, it is just two asks.

So each lens runs its own clock, and an **ask** — one call to one lens — is what resets it. A **quiet
ask** costs one **interval**; an ask that produces an impulse costs **interval + cooldown**. Nothing
keys on speech: *carry* is the trigger. The moment a lens's question lands as a pending impulse, that
lens goes quiet for the cooldown, whether or not the companion has voiced it yet — which is what makes
the anti-nag structural rather than prompt-discouraged. A lens cannot re-raise within its cooldown
because it is never asked, not because the prompt asks it nicely not to.

With the draw gone the tick has nothing left to decide, so it retires too. The Director still passes on
its own schedule, but a pass checks clocks and a pass where nothing is due makes zero LLM calls.
Several clocks expiring together simply produce several asks, and possibly several carries — delivery
absorbs them, which is the multi-carry redesign recorded in ADR 0004 (issue #9) and made necessary
by this one.

## Considered Options

- **Keep the tick and layer per-lens cooldowns on top of the draw.** Rejected: once every lens knows
  when it is due, the tick is a second cadence knob quantizing the first, and a tick whose lenses are
  all on cooldown is a scheduled no-op. Interval already is the throttle.
- **Keep weight as a probability of asking when due.** Rejected: weight existed to decide who wins the
  turn, and lenses no longer compete for one. Its only remaining job was *off* (`weight: 0`, the escape
  hatch for a lens whose prompt needs a mod that isn't installed), and a `bool enabled` says that
  without implying the other 99 values mean something.
- **Key the cooldown on the impulse being spoken rather than carried.** Rejected: when a carry gets
  voiced belongs to delivery (ADR 0004), and speech can lag carry indefinitely — a lens gated on it
  could re-ask about a subject she is already carrying. Carry is the event this loop owns.
- **Roll the forced turn once per pass instead of per ask, to keep the odds constant.** Rejected: the
  roll stays in the template, per-ask, byte-identical. The odds of *some* forced impulse multiply when
  several lenses ask at once; accepted, because extra forced impulses are just extra carries and
  delivery absorbs them.

## Consequences

- **Interval is now both the chattiness knob and the cost knob.** A quiet ask is an LLM call, and there
  is no tick left to throttle it — lengthening a lens's interval is the only way to make it cheaper.
- Shipped cadence, in game hours (interval / cooldown): **Aspiration 2 / 8, Relationship 6 / 24,
  Activity 4 / 48**. Aspiration is the workhorse and asks often. Relationship's material accumulates
  slowly, so a long interval buys fewer quiet asks against the same standing data. Activity's danger is
  repeat-proposing — "spar with me" again tonight — so it takes the longest cooldown of the three.
  Under ADR 0002 these are shipped defaults: cadence fields absent from an existing config take them,
  and a later release that retunes one retunes it for every install that hasn't moved that control.
- The draw's one behavioural guarantee — two consecutive asks never repeat a question — is subsumed,
  not lost: a lens structurally cannot re-ask within its interval, so consecutive asks are different
  lenses or the same lens an interval apart.
- The per-lens overrides become `enabled` and the two cadence values (plus the existing
  `ledgerSlots`), still keyed by `Lens::id` per ADR 0002. Ledger rings still key on `Lens::name`.
- **Config migration, under ADR 0002's deviations-only loader.** A stored `weight: 0` override maps to
  `enabled: false` — that is how installs switched lenses off, and silently re-enabling one whose
  prompt depends on a missing mod would dispatch a prompt that fails to parse. Everything else retired
  here and by ADR 0004 becomes an obsolete key: ignored on load, logged once. The full list, in one
  place: `intervalGameMinutes` (the tick), nonzero per-lens `weight` overrides (the draw), `delivery`
  and `degradeToPersistentEvent` (both retired by ADR 0004).
- The implementation must touch two documents this ADR deliberately does not edit: the README's lens
  section (the "Selection is in the DLL, not the template" paragraph and every mention of weight,
  including *Tuning the impulse*), and the CONTEXT.md glossary — **ask** / **quiet ask** replace
  **tick** / **quiet tick** (the per-lens quiet counts survive as-is), **interval** and **cooldown**
  enter the language as cadence terms, and the Director **pass** takes over the scheduling half of
  what tick meant: a clock check that may make zero calls. Cooldown stays on the Ledger's avoid-list
  — eviction is by count, and the clock term belongs to lens cadence only. The lens-roster entry's
  "a user tunes a lens's weight" clause rewords to the cadence fields and the enable switch.
