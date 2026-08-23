## 1. Stable Lifecycle Foundation

- [x] 1.1 Add persisted `EntryId`, target scope, origin-owned memory identity, and the `Untouched`/`RaisedUnmet`/`Met` state model to the pending interface; migrate every existing pending caller to stable IDs and verify the plugin target compiles.
- [x] 1.2 Implement atomic ID-based carry, raise, meet, stop-carrying, and forget-memory transitions with regression rejection; extend `LedgerTests` to verify stale IDs cannot mutate replacement entries.
- [x] 1.3 Replace boolean-dependent expiry with state-aware retirement and verify tests cover untouched slot withdrawal, raised-memory retention, met removal, and backward game-time jumps.
- [x] 1.4 Version `AgencyEngine_Pending.json`, persist IDs/state/target scope/watermarks plus `nextEntryId` and per-save `nextEvidenceSequence`, create the one-time migration backup, and verify a legacy sidecar fixture migrates, allocates post-reload sequences above persisted watermarks, rewrites, and reloads without losing entries or confirmed personal records.

## 2. Entry-Owned Cue and Bio Presentation

- [x] 2.1 Add entry ownership to coalesced cues and immutable floor grants, preserving newest-wins before emission and fixed ownership afterward; verify focused tests cover multiple lenses, later carries, failed cue dispatch, floor timeout, and save reset.
- [x] 2.2 Mark only the floor owner raised when its companion produces qualifying speech and verify a cue with speech costs zero resolver calls while a silent cue leaves the entry untouched.
- [x] 2.3 Update pending bio decorators and shipped character-bio prompt assets to render one foreground entry, untouched background carries, and raised/unmet threads distinctly; verify prompt rendering never tells a raised entry to introduce itself again.

## 3. Personal and Party Topic Memory

- [x] 3.1 Replace byte-wise topic normalization with strict Windows Unicode normalization/casing and persisted normalized keys; verify Latin, accented, Cyrillic, CJK, compatibility, punctuation, whitespace, invalid UTF-8, and empty-topic cases.
- [x] 3.2 Make personal exact veto inspect every lens while preserving per-lens insertion/eviction, and make provisional duplicate recording unable to downgrade confirmed memory; verify the existing ledger suite plus new cross-lens and confirmed-slot regressions pass.
- [x] 3.3 Add persisted party-heard records with origin ID, speaker, target scope, topic key, display topic, and raise time; verify seven-game-day expiry, 32-record eviction, save/reload, and legacy-empty initialization.
- [x] 3.4 Confirm personal and party memory atomically on raise and retire compatible exact-matching untouched carries owned by other followers; verify raised threads and unequal known target scopes remain intact.
- [x] 3.5 Extend generation context and local candidate veto with party-heard memory, rendering at most 12 newest records while checking all retained records; verify exact cross-follower repeats are rejected without another LLM call and prompt fixtures include the compact semantic self-check section.
- [x] 3.6 Implement entry-specific stop-carrying, forget-subject, and global-forget semantics for both memory layers; verify independent records with the same normalized topic are not removed accidentally.

## 4. Cost-Safe Settings Cutover

- [x] 4.1 Replace `pendingResolveGameMinutes` and the superseded event cap with clamped `pendingResolveEventInterval` defaulting to 30 and `pendingResolveCooldownSeconds` defaulting to 240 real seconds; retain the party-echo default of 7 days and the private-thought default of false; verify settings default and clamp tests pass.
- [x] 4.2 Remove both retired resolver keys from saved output and report them as obsolete when loading legacy configuration without reenabling their behavior; verify a load/save migration test emits only the checkpoint settings and preserves unrelated overrides.

## 5. Evidence-Driven Resolution Scheduler

- [x] 5.1 Add an owned raw event DTO and bounded thread-safe ingestion queue, expand SkyrimNet callbacks to copy minimal source identity, actor/target, type, text, and arrival data before return, then assign accepted events a persisted per-save monotonic sequence while retaining source IDs only for deduplication; verify callbacks perform no resolver or game-thread access and a reload cannot place new evidence below an old watermark.
- [x] 5.2 Bind every raw event, queued result, and in-flight batch to `ActiveSaveToken { saveId, generation }` plus a batch ID, increment generation on every load/new game, and apply results only on the Director thread after a complete-token match; verify save A’s callback cannot mutate save B’s coincidentally identical entry ID, cannot clear B’s in-flight batch, and remains stale after an A-to-B-to-A load sequence.
- [x] 5.3 Implement per-entry accepted-event watermarks and retain cheap meaningful-token/type association only as resolver context hints; verify unrelated events advance untouched and raised/unmet entries, independently newer entries wait for their own checkpoint, and replayed or failed evidence cannot retrigger a call.
- [x] 5.4 Implement recurring event-checkpoint, global real-time cooldown, manual, and pre-expiry triggers with one batch in flight; verify 29 events do not dispatch, event 30 does, busy dialogue cannot postpone it, rapid second intervals wait for four minutes without losing evidence, manual work bypasses both gates, and queued fallbacks remain expiry-protected.
- [x] 5.5 Replace the boolean resolver prompt contract with shared events plus ID-keyed entry arrays and independent tri-state verdicts; verify parser tests apply valid token-matched partial output while ignoring missing, duplicate, unknown, stale-save, stale-batch, malformed, and regressive verdicts.
- [x] 5.6 Integrate the scheduler into the Director, remove recurring per-entry polling and FormID+lens resolver queues, and reorder final fallback before state-aware expiry; verify time-skip, queue refusal, request failure, callback failure, save transition, and evidence-arriving-during-flight tests leave no stuck entry or in-flight token.
- [x] 5.7 Convert “check this/all now” into one explicitly paid ID-based manual batch with duplicate-click coalescing; verify selected replacement entries cannot be cleared by an older queued request.
- [x] 5.8 Add a 15-active-real-second bounded `PublicGetRecentEvents` recovery poll with per-save baseline establishment, shared callback source-ID deduplication, UUID/text/time parsing, and no main-thread or LLM work; verify missed callbacks advance exactly once, callback-plus-poll duplicates do not advance twice, historical load tails do not advance, and polling pauses with normal Director work.

## 6. Diagnostics and UI Parity

- [x] 6.1 Publish stable lifecycle, floor owner, memory-layer, evidence, queue, trigger, zero-call transition, paid-batch, classified-entry, stale-verdict, and overflow diagnostics through one snapshot; verify snapshot tests remain internally consistent during an in-flight batch.
- [x] 6.2 Update SKSE Menu Framework settings, carried, ledger, history/diagnostic, details, and actions for the new states and controls; verify the live plugin surface shows stable IDs, two memory layers, paid manual actions, accepted-event interval and real-time cooldown help, and party retention.
- [x] 6.3 Update MCM native accessors and `AgencyEngine_MCM.psc` with identical checkpoint settings, paginated/detail views, diagnostics, and actions; compile the Papyrus script and verify every changed SKSE Menu Framework datum/action has an MCM counterpart with equivalent wording and behavior.
- [x] 6.4 Publish recovery-poll cadence, elapsed time, tail size, newly recovered count, and failure diagnostics through the shared status snapshot and both SKSE Menu Framework and SkyUI MCM views with equivalent wording.

## 7. Documentation and Decision Record

- [x] 7.1 Add ADR 0006 covering stable identity, entry-owned floor grants, three-state lifecycle, two-layer memory, and evidence-driven batching; verify it resolves ADR 0004’s open attribution/resolution questions without contradicting prior per-lens decisions.
- [x] 7.2 Update `CONTEXT.md`, `README.md`, behavioral specs, prompt documentation, setting examples, and user-facing cost guidance; verify searches find no documentation describing timer polling, actor-wide cue attribution, raised topics as unsaid, or private thoughts as default-on.
- [x] 7.3 Verify source and staged SkyrimNet prompt copies, MCM source, native bindings, and packaged assets all carry the same batch schema, decorator names, setting names, and lifecycle vocabulary.

## 8. Integrated Verification

- [x] 8.1 Build the plugin and Papyrus assets from a clean configured tree and verify there are no compile, link, script, or packaging errors.
- [x] 8.2 Run the complete offline test suite and verify lifecycle, ledger, Unicode, settings, scheduler, migration, stale-callback, and existing combat/cadence contracts all pass.
- [x] 8.3 Smoke a scripted event sequence through the built scheduler—two followers, several carried entries, an ignored raise, a deferred proposal, a met topic, duplicate evidence, and a failed batch—and verify call counters, final pending states, and both ledgers match the specs.
- [x] 8.3a Replace global conversation-settle resolution with persisted 30-event checkpoints and a four-real-minute automatic cooldown; verify unrelated events advance both untouched and raised/unmet entries, independently newer entries wait for their own checkpoint, manual checks bypass both gates, and both menu implementations expose identical controls.
- [ ] 8.4 In-game, exercise untouched, raised/unmet, met, cross-lens, cross-follower, save/reload, time-skip, repeated 30-event checkpoints, automatic cooldown, and failed-batch paths; verify busy dialogue cannot postpone a due check, calls remain rate-limited, and retained exact memory prevents repeated subjects.
- [ ] 8.5 In the same in-game build, inspect and operate SKSE Menu Framework and SkyUI MCM side by side; verify settings, pagination/detail dialogs, states, ledger records, diagnostics, and stop/forget/manual actions have behavioral parity before release.
