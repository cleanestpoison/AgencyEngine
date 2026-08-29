## Context

See `proposal.md` for motivation and `specs/impulse-generation/spec.md` for the behavior contract.

Every built-in lens extends `agencyengine_impulse_base.prompt` and overrides prose blocks. The child-file `{# ... #}` comments are stripped by Inja and do not affect model behavior; rendered task text, eligibility lists, examples, subject-source rules, and the final user question do. Activity and Relationship currently repeat loaded subjects across several of those rendered blocks. The same grandstanding scenario is also repeated as cross-lens routing guidance.

The DLL supplies state and settings to each independently scheduled lens ask. A forced roll is evaluated in the shared template per ask. Settings persistence records deviations from shipped values, so changing a shipped default reaches installations that never changed that control while preserving explicit overrides. Prompt rendering and model judgment have no offline test seam; current verification uses in-game history context, per-lens counters, carried entries, and generated dialogue.

## Goals / Non-Goals

**Goals:**

- Make each built-in prompt a boundary-and-evidence question rather than a repertoire-completion exercise.
- Reduce rendered instruction size while retaining the JSON contract, reaction boundary, anti-repeat rules, and state dump.
- Ensure motive and emotional-register inference require actor-state support.
- Retune shipped defaults through the existing deviations-only settings mechanism.
- Keep SKSE Menu Framework and SkyUI MCM behaviorally equivalent.

**Non-Goals:**

- Making companions uniformly friendly, agreeable, or noncompetitive.
- Adding sentiment scoring, subject blacklists, output post-processing, or a second model call.
- Changing lens intervals, cooldowns, pending lifecycle, proposal resolution, cue ownership, or ledger behavior.
- Changing user-authored lens contracts or overwriting explicit user settings.
- Expanding the per-follower thought limit or changing event filtering semantics.

## Decisions

### Replace rendered catalogs with one semantic criterion per lens

Activity will be reduced to a question about a voluntary shared experience whose payoff is the experience or company rather than an errand, obligation, or relationship reckoning. Relationship will be reduced to a question about something presently relevant in how the speaker relates to a person present rather than a plan or activity. The state chooses the specific subject and register.

The lens task, eligibility block, focus, subject-source block, and final question will each express only the part of that boundary needed at that location. They will not repeat example subjects. This preserves the inherited block structure and avoids introducing a second prompt convention.

**Alternative considered:** balance every competitive or hostile example with a warm example. Rejected because a larger balanced catalog still constrains the model to a maintainer-authored repertoire and increases prompt weight.

### Remove loaded examples instead of adding counterexamples

Activity and Relationship content-bearing examples will be removed. Cross-lens examples in Aspiration and Activity that reproduce loaded Relationship material will also be removed or reduced to a content-free boundary statement. Where the inherited `lens_examples` heading needs content, the override will use the existing Curiosity pattern: state that no subject examples are supplied and describe only output shape.

The shared base's generic output-contract examples remain only where they teach structural correctness rather than a preferred social subject. Existing invalid-format examples remain eligible if they demonstrate JSON, dialogue, or staging failures without advertising a lens subject.

**Alternative considered:** retain examples but rotate characters and subjects. Rejected because negative examples still prime their content and rotation only changes which repertoire is overrepresented.

### Strengthen evidence by rewriting an existing base constraint

The shared “use only what the state supports” constraint will be rewritten, not supplemented with a new section, to cover subject, emotional register, and attributed motive. A recorded action may support mentioning that action; it does not by itself support the speaker deciding why another person did it. This rule applies consistently to built-in and user-authored lenses inheriting the base.

Lens-specific prose will point to state selection without restating the full rule. Sharp, warm, neutral, and silent outcomes remain valid when supported.

**Alternative considered:** validate model output with a motive classifier or blacklist. Rejected because it adds cost and machinery, cannot reliably distinguish characterization from invention, and treats symptoms after generation rather than removing the prompt bias.

### Render shared contracts outside overridable blocks

Live rendered context showed that SkyrimNet's Inja renderer does not reliably emit an unoverridden base block body from an extending template. Required shared contracts therefore remain ordinary base text rather than fallback block bodies. Speaker eligibility, the default target domain, the stamped-evidence timing floor, and the final evidence door always render.

Lens-specific blocks are explicit requirements for every built-in lens. Optional variation uses empty additive hooks: Curiosity alone adds a current-state timing exception and narrows its target to the player. This keeps the shared spine as the deep module while making its interface match the renderer's observed behavior.

**Alternative considered:** duplicate the missing default bodies across every built-in lens. Rejected because it leaves custom lenses exposed to the same false fallback contract and lets shared evidence wording drift.

### Keep forcing available but ship it disabled

`forcedImpulseChance` remains a 0–100 user setting and the existing per-ask template branch remains intact. Only the shipped default and default-facing text change from 20 to 0. An absent setting therefore keeps silence available; an explicit nonzero setting preserves the user's chosen forcing behavior.

The template fallback for DLLs too old to inject the value will also become 0 so old integration cannot silently reintroduce forcing.

**Alternative considered:** prohibit forcing only for Relationship. Rejected for this change because it introduces per-lens forcing semantics and configuration not requested by the evidence; shipping 0 removes default multiplication across all lenses without removing user control.

### Increase only the total event budget

The shipped `maxEvents` value becomes 70. Its valid range, event ordering, filters, and `perFollowerEvents` value remain unchanged. This broadens the fetched state window without creating a second event-budget concept.

**Alternative considered:** increase the per-follower thought budget at the same time. Rejected because it changes prompt composition and token cost independently of the requested total-window correction.

### Update defaults through the existing single sources of truth

The settings struct remains the runtime source of shipped scalar defaults and the built-in lens table remains unchanged. Configuration examples, both UI presentations, tests, and documentation will be updated to agree with those values. Persistence behavior will not gain a migration branch: absent keys adopt 0 and 70; explicit keys remain deviations even when they equal the former defaults.

## Risks / Trade-offs

- **[Risk] Less exemplification produces more quiet asks or less predictable subjects on weaker models.** → Keep the semantic boundary, evidence gate, output schema, and forced control; verify using multiple in-game asks and per-lens carried/quiet counters rather than restoring subject catalogs.
- **[Risk] A 70-event request increases context size, latency, and model cost.** → Leave per-follower limits and filtering unchanged, inspect actual rendered history context in game, and document the tunable event budget on both interfaces.
- **[Risk] Older events can support stale associations.** → Preserve stamped `why_now`, live-tail exclusion, recent-repeat checks, and the requirement that register and motive be evidenced rather than inferred from action alone.
- **[Risk] Removing examples may weaken output formatting.** → Preserve the shared JSON schema and structural bad-output guidance; remove only content-bearing social examples.
- **[Risk] Rewriting the shared evidence constraint also affects custom lenses.** → Keep the source domains and output contract unchanged; narrow only unsupported motive and register invention, which is already intended by the existing “nothing invented” rule.
- **[Risk] Prompt quality cannot be established by source-text unit tests.** → Do not add brittle word-blacklist tests. Use in-game rendered context and behavior, while automated tests cover deterministic settings and persistence contracts.

## Migration Plan

1. Ship the prompt assets, settings defaults, both UI updates, configuration example, tests, and documentation together.
2. On load, installations without explicit deviations adopt `forcedImpulseChance = 0` and `maxEvents = 70` through existing defaults.
3. Installations with explicit values retain them unchanged; no config rewrite or sidecar migration runs.
4. Verify a fresh/default configuration and an explicit-override configuration in both interfaces.
5. Rollback restores the previous assets and shipped defaults; explicit user values remain readable because keys and ranges do not change.
