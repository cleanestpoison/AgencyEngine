# Proposals are a distinct kind of impulse from topics

The loop was built around impulses whose payoff is the talking: she raises her father, you answer,
the beat is complete. The Activity lens breaks that assumption — it produces *proposals* ("spar with
me", "come drink"), where the talking is not the payoff and agreement alone means nothing has
happened yet. So a lens now declares whether it produces topics or proposals, and both the resolution
check and the ledger branch on that declaration.

## Considered Options

- **Infer it from the lens name in the prompt** (`{% if lens == "Activity" %}`). Rejected: `Lens::name`
  is a free-text field on the Settings page. A rename silently reverts the branch to topic semantics
  with no error and no log line — it fails as wrong behaviour, not as a failure.
- **Let the resolution prompt judge from the stage direction.** Rejected: costs an inference per check
  and splits on the ambiguous middle ("she asks whether they are stopping in town tonight" — a want,
  or a proposal for tonight?).
- **Ship the Activity lens without touching resolution or the ledger.** Rejected once the failure was
  concrete: agreement resolves the impulse, the ledger slot is confirmed, the subject is suppressed for
  six beats, and the sparring never happened. The one case that should keep her carrying it is the one
  that clears it fastest.

## Consequences

- `Settings::Lens` gains `bool proposal` and a per-lens `ledgerSlots` override; the flag rides
  `PendingImpulses::Entry` into the resolution context. Adding a settings field means touching
  `Settings.h`, `Load`, `Save`, `Summary` and the UI, per the repo conventions.
- `agencyengine_impulse_resolved.prompt` branches on `proposal`: agreement is a deferral until the
  events show the thing happened or someone plainly refused. This reuses the existing "deferral is not
  an answer" rule rather than introducing a new verdict.
- **Ledger eviction and veto scope per lens.** The ledger's count-based eviction with no clock is
  correct for topics — a quiet in-game week must not make a grievance raisable again — and wrong for
  proposals, which come from a closed vocabulary of under ten subjects. A shared six-slot ring would
  hold the Activity lens's entire repertoire and veto it into permanent silence, *and* evict genuine
  Aspiration and Relationship subjects to do it, degrading two lenses that already shipped. Each lens
  now evicts only within its own ring. Rendering of "already raised" stays combined, so no lens repeats
  another's subject.
- `AgencyEngine_Pending.json` gains a `lens` key on each ledger slot. Slots restored without one are
  legacy and keep today's shared-ring behaviour.
