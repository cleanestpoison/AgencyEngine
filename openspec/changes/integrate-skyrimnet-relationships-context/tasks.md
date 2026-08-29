## 1. Optional Decorator Compatibility

- [x] 1.1 Extend the data-load compatibility registration in `plugin.cpp` to detect `SNRom_Integration.esl`, leave real Relationships decorators untouched when present, and register parse-safe `get_romance` and `romance_physical_ok` stand-ins when absent; verify `pwsh -File build.ps1 build` succeeds and logs identify the selected path.

## 2. Shared Relationships Standing Context

- [ ] 2.1 Add the enrolled-versus-fallback source selection to `agencyengine_impulse_base.prompt`, preserving the existing SeverActions player passage for unenrolled companions and the independent companion-opinions passage in all cases; verify History context from manual asks shows exactly one player-standing source per follower.
- [ ] 2.2 Implement the concise six-stage platonic and romantic summaries, stance branches, known-only authored facets, separate romantic/physical availability, permanent-versus-present physical boundaries, and fixed-limit precedence; verify enrolled fixture states render qualitative facts without points, tier numbers, level names, `toNext`, raw disposition values, address, or invented defaults.
- [ ] 2.3 Add the complete 58-entry Romantasy preference vocabulary and one-pass rank rendering, emitting polarity only for established rank 0/1 opinions; verify a follower with mixed opinions renders every authored like/dislike once and omits unopinionated activities.
- [ ] 2.4 Add the minimum shared and lens-specific instructions needed to keep Relationships state read-only and within each lens's existing subject boundary; verify Aspiration, Relationship, Activity, and Curiosity retain their existing output contract and no prompt claims to mutate spark, stance, bond, or progression.

## 3. Documentation and Automated Verification

- [x] 3.1 Update project documentation to credit SkyrimNet Relationships, state the optional 1.1.2+ support boundary, explain enrolled-source precedence and fallback behavior, and note that no setting or UI is added; verify documented names, plugin filename, decorator contract, and exclusions match the design and packaged files.
- [x] 3.2 Run `pwsh -File build.ps1 test -Preset local-tests` and `pwsh -File build.ps1 build`; verify all existing tests pass and the release plugin builds with the fallback registration changes.

## 4. In-Game Prompt Verification

- [ ] 4.1 Launch without SkyrimNet Relationships, manually ask all four lenses, and verify every prompt renders without an unknown-decorator error while preserving SeverActions or neutral player standing.
- [ ] 4.2 Launch with SkyrimNet Relationships 1.1.2+ and an unenrolled companion, manually ask all four lenses, and verify the existing SeverActions player fallback and companion opinions remain intact.
- [ ] 4.3 Launch with Relationships-enrolled companions covering platonic and romantic depths, accepted/declined/unanswered stance, temporary/permanent physical unavailability, a fixed limit, known and missing optional facets, and mixed preferences; inspect History context and verify every specified fact, omission, source-precedence rule, and boundary distinction.
- [ ] 4.4 Exercise manual asks for Aspiration, Relationship, Activity, and Curiosity with enrolled context, then observe carried impulses through ordinary dialogue; verify cadence, forced-turn handling, pending lifecycle, cue delivery, and resolution remain behaviorally unchanged and no Relationships transition is initiated or recorded by AgencyEngine.
- [ ] 4.5 Compare prompt render timing with representative multi-follower parties before and after enabling enrolled preference context; verify the single 58-rank pass adds no material response-delay regression, recording the observed timings for release review.