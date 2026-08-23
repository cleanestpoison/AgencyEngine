## Context

See `proposal.md` for motivation and the three delta specs for behavioral contracts.

Today `PendingImpulses::Entry` is selected by companion FormID plus lens, carries a `spoken` boolean, and stores the game time of its last per-entry check. The same module owns provisional/confirmed per-lens ledger slots and an actor-only 30-second floor marker. `Director` coalesces cues by actor, polls one pending entry every configured 180 game minutes, fetches that follower’s recent event tail, and applies a boolean resolver result captured by FormID plus lens. SkyrimNet dialogue callbacks currently retain only the latest timestamp.

Constraints shaping the replacement:

- SkyrimNet invokes event and LLM callbacks on its ThreadPool. Callback `const char*` payloads expire on return, and callback code cannot access `RE` objects.
- Prompt generation and the SkyrimNet decorator can read pending state concurrently with the Director and UI. The decorator path must stay short and bounded.
- Each companion may carry one entry per lens, several lenses may coalesce behind one cue, and ledger eviction must remain per lens.
- Resolution and private-thought calls are paid. Irrelevant conversations and replayed evidence must cost zero calls.
- AgencyEngine is Windows-only, persists pending state in `AgencyEngine_Pending.json`, and must preserve live saves through the format change.
- SKSE Menu Framework and SkyUI MCM must expose equivalent behavior in the same release.

## Goals / Non-Goals

**Goals:**

- Make pending identity and every asynchronous operation stable across supersession and reload.
- Make “raised” an observed, durable anti-repeat transition separate from whether the thread was met.
- Deepen the pending-state boundary so ledger mutation, lifecycle transitions, and stale-result rejection are atomic and offline-testable.
- Make automatic resolver cost proportional to new relevant evidence, not elapsed game time or generic follower participation.
- Share event context and one LLM request across every eligible entry while bounding prompt size and retries.
- Preserve personal lens-ring retention while adding a short-lived party echo guard.

**Non-Goals:**

- Embeddings, a second duplicate-classifier call, or any other semantic-memory dependency.
- A permanent union of every companion’s personal ledger.
- Changing lens cadence, impulse-generation prompts beyond their memory context, or the one-entry-per-companion-per-lens rule.
- Emitting one narration per carried entry or naming a subject in the visible coalesced cue.
- Reconstructing historical party-heard records that predate this release; their raise time and target scope are unknowable.

## Decisions

### 1. Keep lifecycle and memory atomic; extract resolution scheduling

`PendingImpulses` remains the owner of entries, personal ledger records, party-heard records, floor grants, and their sidecar. Its public boundary changes from mutable-slot operations to ID-based commands:

```text
Carry(entry without ID) -> EntryId
MarkRaised(EntryId, raisedAt)
MarkMet(EntryId)
Retire(EntryId, PreserveConfirmed | ForgetOwnedMemory)
Find/Snapshot by EntryId
GrantFloor(EntryId, actor, deadline)
```

All entry and ledger changes caused by one command occur under the existing state lock, then persistence is marked dirty. A provisional record is therefore confirmed, a party record is inserted, and matching untouched echoes are retired as one state transition. No external caller composes these invariants from separate ledger calls.

A new `ResolutionScheduler` module owns copied evidence, relevance selection, evidence cursors, batching, in-flight state, and resolver response parsing. `Director` drains/pumps this module on its existing pass and supplies game snapshot/settings; `SkyrimNetAPI.cpp` remains the only translation unit including `PublicAPI.h` and forwards copied callback evidence into it.

The scheduler also owns an `ActiveSaveToken { saveId, generation }`. `generation` is process-local and increments on every new-game/load transition, including loading the same save again. `BeginSave` changes the token and clears queued evidence, eligibility, and the optional current batch under the scheduler lock. Event callbacks stamp raw work with the token current under that same lock. LLM callbacks copy their response into a result queue; only the Director thread validates and applies results, serializing them with save transitions.

```text
SkyrimNet worker                Director thread                 SkyrimNet worker
      |                               |                                |
      | copy minimal event            |                                |
      +--> ResolutionScheduler queue  |                                |
                                      | drain + associate              |
                                      | factual transitions ----------> PendingImpulses
                                      | build one eligible batch       |
                                      +-------------------------------> LLM
                                                                       |
                                      PendingImpulses <--- ID verdicts-+
```

This seam keeps the callback trivial, keeps `Director` from accumulating another state machine, and makes the cost policy testable without Skyrim or an LLM. Keeping all scheduling inline in `Director` was rejected because evidence buffering, retry suppression, and callback correlation would couple unrelated cadence and delivery code. Moving ledger ownership into the scheduler was rejected because it would split one lifecycle transition across locks and persistence owners.

### 2. Use a persisted origin ID and a monotonic lifecycle enum

Introduce `using EntryId = std::uint64_t` and persist `nextEntryId` per save. `Carry` allocates under the pending lock and returns the ID; IDs are never reused. Every personal or party memory record stores `originEntryId`, so confirmed history remains attributable after its pending row disappears. Legacy confirmed records receive a synthetic historical origin ID during migration.

Replace `spoken` and `lastCheckGameDays` with:

```text
state: Untouched | RaisedUnmet | Met
raisedGameDays
lastAttemptedEvidenceSequence
fallbackConsumed
resolutionInFlightThroughEvidenceSequence (transient)
```

`Met` is a terminal transition: it confirms memory and removes the entry in the same command, so it need not remain in normal snapshots. Transition functions reject regressions and missing IDs. Entry IDs are unique only within one save, so asynchronous resolver identity is `BatchToken { ActiveSaveToken, batchId }` plus entry IDs and the evidence upper bound; FormID plus lens is never identity.

The state table is:

```text
Current         Evidence/action                 Pending result        Memory result
Untouched       owned speech observed           RaisedUnmet           confirm personal + add party
Untouched       met without own raise            Met / remove          confirm personal + add party
Untouched       TTL                              remove                withdraw owned provisional
RaisedUnmet     relevant but still outstanding   RaisedUnmet           retain confirmed
RaisedUnmet     met                              Met / remove          retain confirmed
RaisedUnmet     TTL or stop carrying             remove                retain confirmed
Any open state  explicit forget subject          remove                remove records owned by origin ID
```

A boolean plus separate flags was rejected because it permits impossible combinations and makes regression checks distributed. Using actor+lens generations instead of IDs was rejected because generations would still have to be persisted and threaded through every caller, while being less useful to UI and ledger diagnostics.

### 3. Freeze one exact owner when a coalesced cue is emitted

Add `entryId` to `PendingCue`. Each newer carry for that actor updates an unissued cue’s owner and target while retaining the original defer clock and incrementing the visible carry count. Successful narration dispatch replaces the actor-only floor marker with:

```text
FloorGrant {
  entryId,
  speakerFormID,
  target scope,
  granted steady-clock time,
  deadline
}
```

The existing `agencyengine_has_the_floor` decorator remains an actor-level boolean because the prompt only needs to know whether the companion may open. Scheduler attribution uses the exact grant. Once emitted, a grant is immutable until dialogue settle or its real-time deadline; a subsequent carry may create the next cue but cannot steal the active floor.

The first qualifying dialogue event from the granted speaker marks that entry `RaisedUnmet` for zero LLM calls. Other carried entries still render in the background block but do not receive “raise now” wording and are not marked raised from this grant. The bio decorator gains an entry-aware foreground rendering internally while keeping its external decorator names stable for SkyrimNet templates.

Making the visible cue name the entry was rejected because coalescing and natural dialogue require vague narration. Granting a cohort was rejected because it recreates the attribution ambiguity and lets one line confirm several unrelated topics.

### 4. Store personal and party memory as separate bounded collections

Personal records keep the current per-companion, per-lens rings and their configured capacities. Exact veto changes to inspect all rings for the candidate companion, while insertion and eviction remain confined to the owning ring. Records store the normalized key once; suppression no longer renormalizes every stored topic on every query.

Party-heard records form a separate save-scoped chronological collection:

```text
PartyTopic {
  originEntryId,
  speaker identity,
  target scope,
  displayTopic,
  normalizedKey,
  raisedGameDays
}
```

A known target matches only the same target; an unknown target is party-wide. On `MarkRaised`, the state module:

1. confirms the origin’s personal record;
2. inserts or refreshes the party record;
3. retires compatible exact-matching `Untouched` entries owned by other companions and withdraws their provisional records;
4. leaves already raised entries intact.

Party records expire after `partyEchoGameDays` (default 7) and are capped at 32 oldest-first. Local veto checks all 32; impulse-generation context renders only the 12 newest in one compact party section. Personal records retain their existing count-based lifetime, so party expiry never erases what a specific companion remembers.

One permanent shared ledger was rejected because it would indefinitely silence distinct relationship perspectives. Sharing provisional carries was rejected because one follower privately considering a topic should not suppress everyone else. Merely enlarging existing rings was rejected because it does not represent who heard a subject and does not fix raised/unmet state.

### 5. Normalize a topic once with Windows Unicode services

No new library is required. The plugin already targets Windows, so normalization uses strict UTF-8 conversion plus Win32 Unicode APIs:

1. `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)`;
2. `NormalizeString(NormalizationKC)`;
3. `LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING)`;
4. `GetStringTypeW` to retain Unicode letters/numbers and collapse all other runs to one ASCII space;
5. strict UTF-16-to-UTF-8 conversion.

The display topic remains untouched. Invalid UTF-8 or a genuinely empty normalized key rejects ledger recording and emits a diagnostic. Migrated records compute and persist a key once.

ASCII `std::isalnum` was rejected because it drops non-Latin topics and is locale-sensitive per byte. ICU/utf8proc were rejected for this Windows-only requirement because the OS supplies the needed normalization and casing without adding package, binary, or allocation overhead beyond the conversion itself.

### 6. Treat recoverable accepted-event checkpoints and persisted watermarks as the cost boundary

SkyrimNet's public callback is the low-latency observation path, but it is not the sole source of truth: supported builds can register callbacks successfully without delivering them. The Director therefore polls the player's bounded 100-event recent tail every 15 active real seconds through `PublicGetRecentEvents`. This cadence runs on the Director thread, never while normal work is suspended, and is deliberately fixed rather than user-configurable. Each poll records elapsed time, returned count, newly recovered count, and failures so in-game diagnostics expose unexpected event-store contention.

The callback and polling adapters feed one owned raw-event interface. Callback payloads copy only fields available without game access. Poll payloads map SkyrimNet's stable source event ID, type, nested text, actor/target UUIDs, and wall-clock event time into the same DTO; wall-clock age is converted to a steady arrival time at the poll boundary. The first poll after each active-save transition seeds a bounded source-ID baseline without emitting evidence. Callback source IDs observed before a poll seed that same cursor. Later tail observations emit each unseen source ID once, so callback recovery cannot duplicate an evidence sequence or a floor-owned raise.

`EnqueueRaw` stamps each emitted event with the scheduler's current `ActiveSaveToken` under the queue lock. When the Director drains that queue, it discards token mismatches, performs the final source-ID deduplication, and assigns every accepted event the next per-save monotonic `evidenceSequence`. `nextEvidenceSequence` is persisted and never resets on reload; source IDs remain separate dedupe keys and never determine ordering or entry watermarks.

Each entry records `lastAttemptedEvidenceSequence` when carried. Every verified open entry becomes due after `pendingResolveEventInterval` newer accepted events, regardless of local topic wording or participant identity. Both `untouched` and `raised_unmet` entries use the same checkpoint: a carried concern can become moot through later events before it is introduced, while an introduced concern can be answered immediately and classified at the next checkpoint. A floor-owned raise remains a zero-call lifecycle transition and does not reset the entry's watermark.

At dispatch, the Director records the batch's highest covered `evidenceSequence` on every attempted entry. Failure, malformed output, or a stale verdict leaves lifecycle state unchanged but does not let the same interval trigger again. Newer events accumulate toward the next checkpoint and older bounded history may still accompany them as semantic context. Manual “check” actions are explicit paid overrides: selected IDs dispatch without waiting for either scheduling gate, while repeated clicks queued or in flight coalesce.

Local topic matching was rejected as a dispatch gate because carried concerns can be settled indirectly or by paraphrase. Global conversation settlement was rejected because unrelated ambient dialogue continually refreshes SkyrimNet's dialogue clock and can starve resolution in an active game. Reading SkyrimNet's private database was rejected because it couples AgencyEngine to internal schema and locking. Accepted-event progress through the public bounded-tail interface is observable, recoverable, persisted, independent of silence, and naturally avoids calls when the world produces no new evidence.

### 7. Dispatch due entries every 30 events, rate-limited globally

The scheduler maintains a chronological evidence window and compares its latest sequence with each entry's persisted watermark. It dispatches automatically only when both conditions hold:

```text
latestEvidenceSequence - entry.lastAttemptedEvidenceSequence >= pendingResolveEventInterval
real time since the previous automatic paid batch >= pendingResolveCooldownSeconds
```

The shipped interval is 30 events and the shipped cooldown is 240 real seconds. One batch contains every entry independently due at that sequence across all followers and lenses; a newer entry that has not accumulated its own interval is not pulled forward or reset merely because another entry paid for the request. A manual batch bypasses both gates. A proposal's one pre-expiry fallback bypasses the event interval but observes the automatic cooldown.

Only one batch is in flight for the active save. Dispatch allocates a process-local `batchId` and captures `BatchToken { ActiveSaveToken, batchId }`. The worker callback only enqueues its copied result with that token. On the Director thread, a result may apply verdicts or clear in-flight state only when its complete token matches the optional current batch; an old callback therefore cannot clear a newer save's batch. Evidence arriving during a request remains queued for the next checkpoint. The event interval advances monotonically; conversation, combat, menus, and unrelated activity never reset it.

The request includes shared evidence once and entry-specific references into it:

```json
{
  "entries": [
    {
      "id": 104,
      "speaker": "Lydia",
      "target": "Dragonborn",
      "topic": "a drink",
      "impulse": "...",
      "kind": "proposal",
      "state": "raised_unmet",
      "marked_at": "day 7, 02:19",
      "relevant_event_ids": [218, 219]
    }
  ],
  "events": [
    { "id": 218, "type": "dialogue_npc", "actor": "Lydia", "text": "..." },
    { "id": 219, "type": "dialogue_player_text", "actor": "Dragonborn", "text": "..." }
  ]
}
```

The resolver returns `{ "verdicts": [{ "id": 104, "status": "raised_unmet" }] }`. Parsing is per element: a valid current ID applies even when another element is absent or malformed; duplicate, unknown, regressive, or stale IDs are ignored. The prompt continues to distinguish topics from proposals and treats agreement without performance as unresolved.

If an owned raise has no response or relevant action, factual state already says `RaisedUnmet`; no entry is placed in a semantic batch. All eligible entries across followers share one request, but all *open* entries do not: including unaffected entries would increase tokens and let unrelated conversations create false judgements.

### 8. Run one fallback before expiry, then apply state-aware retirement

Timer polling is removed. For a raised proposal lacking decisive evidence, the scheduler creates at most one pre-expiry synthetic marker. Markers due in the same Director pass batch together. The pending pump order becomes:

```text
sync and verify persisted state
expire party-heard records
ingest/associate evidence
schedule due final fallbacks
pump an eligible resolution batch
expire entries not held by an in-flight final batch
publish UI snapshot
```

An entry awaiting its dispatched final batch is held only until that callback completes or fails. Failure consumes the fallback marker and releases normal expiry. Untouched entries do not receive a paid fallback: absent observed dialogue, there is no new conversational fact to classify. Raised non-proposal topics rely on relevant continuations and state-aware expiry because retaining their confirmed ledger already solves repetition.

Keeping the three-game-hour poll as a safety net was rejected because it preserves the highest-cost failure mode. Performing a final check after expiry was rejected because current ordering can erase both the entry and its provisional evidence before the result arrives.

### 9. Version and migrate the sidecar in place

Add a sidecar format version and persist per save:

```text
nextEntryId
nextEvidenceSequence
entries (state, origin ID, raised time, watermarks, fallback flag, target scope)
personalLedger (origin ID, normalized key, confirmation)
partyLedger (origin ID, target scope, normalized key, raised time)
```

`ActiveSaveToken::generation` and the batch serial are deliberately not persisted: they guard callbacks that cannot survive process exit. They reset only with the process, while every in-process load increments generation before pending state can be replaced.

On first load of the legacy representation:

- assign IDs in deterministic stored order and set `nextEntryId` above every assigned value;
- initialize `nextEvidenceSequence` above every migrated watermark, or at 1 when the legacy save has none, so the first post-reload event is always newer;
- map `spoken=false` to `Untouched` and `spoken=true` to `RaisedUnmet`;
- match a legacy provisional record to a live entry by actor, lens, and normalized topic, copying that entry’s ID;
- assign a synthetic historical origin ID to confirmed records with no live entry;
- drop `lastCheckGameDays` after migration;
- start party-heard memory empty because historical raise time and target scope cannot be recovered safely;
- preserve existing FormID/name verification and save pruning;
- mark the save dirty and rewrite it in the new format.

Before the first format rewrite, retain a one-time `.bak` of the sidecar for rollback. The new writer does not dual-write legacy fields. Rolling back the DLL therefore requires restoring that backup or accepting reset pending state; settings rollback uses the older binary’s shipped timer default because new saves omit the retired key.

### 10. Cut over settings, prompts, diagnostics, and both UIs together

Replace `pendingResolveGameMinutes` with `pendingResolveEventCap` (default 30, clamped to a safe positive range) and add `partyEchoGameDays` (default 7). `generateThought` changes its shipped default to false but remains selectable. Loading the retired timer key logs it as obsolete; saving follows the existing deviations-only policy and omits it.

The resolver prompt changes from one entry plus `resolved: bool` to the batch contract above. Character-bio prompts render a foreground “raise this now” entry separately from untouched background carries and raised/unmet threads. Each impulse-generation prompt receives personal ledger context plus the compact party-heard section. The exact local veto remains authoritative for normalized equality.

Both UI presentations change in the same slice:

- **Settings:** event-cap meaning, seven-day party echo window, private-thought cost/default.
- **Carried/open entries:** stable ID, `carried/unobserved` versus `raised/unmet`, floor owner, evidence watermark, fallback state, explicit paid manual-batch wording, stop-carrying versus forget-memory actions.
- **Ledger:** personal layer/ring/confirmation and party layer/speaker/target/age, with pagination/detail dialogs in MCM.
- **Diagnostics:** copied/eligible event counts, batch trigger/in-flight state, zero-call raises, paid batches, entries classified, ignored stale verdicts, and queue overflow.

The evidence queue is bounded; overflow drops oldest non-referenced evidence, increments a diagnostic, and leaves affected raised proposals eligible for their final fallback rather than blocking the Director. Logs name entry IDs, trigger, evidence range, batch size, context bytes, and every ignored result.

An ADR (`docs/adr/0006-...`) records the identity, lifecycle, two-layer memory, and evidence-driven batching decisions. `CONTEXT.md`, `README.md`, prompt documentation, and existing specs use the same vocabulary.

## Risks / Trade-offs

- **[Owned speech can confirm the wrong topic if the model ignores its exact floor instruction]** → Restrict the factual transition to an immutable entry-owned floor, log the spoken event and ID, show it in diagnostics, and provide an explicit forget-memory action. This chooses occasional over-suppression over repeated claims that an actually voiced subject is unsaid.
- **[Cheap relevance misses a heavily paraphrased continuation]** → Use original topic plus meaningful tokens and event types, keep a single pre-expiry proposal fallback, and preserve confirmed memory even when the open thread later expires.
- **[Cheap relevance is too broad and creates calls]** → Require new evidence, direct speaker/target participation, per-entry watermarks, and settle coalescing; generic actor participation never marks all entries dirty.
- **[A batch becomes large with several followers]** → Include only eligible entries, share events once, cap the evidence window at 30, cap party prompt context at 12, and log payload bytes. Split only if the configured model’s hard context limit is exceeded.
- **[Party memory suppresses a legitimate second perspective]** → Scope known targets, keep durable memory personal, expire party echoes after seven game days or 32 records, and never share untouched carries.
- **[Unicode behavior differs across Windows Unicode-table versions]** → Persist computed keys, use invariant OS APIs, and lock representative Latin, accented, Cyrillic, CJK, compatibility, punctuation, and invalid-UTF-8 cases in tests.
- **[Time skip reaches TTL while a final batch is asynchronous]** → Schedule fallback before expiry, mark only the exact IDs held by that batch, and release the hold on every callback and queue-failure path.
- **[A callback from another save carries an entry ID reused by the current save]** → Stamp raw work and batches with save ID plus load generation, apply results only on the Director thread when the complete batch token matches, and never let a rejected callback clear current in-flight state.
- **[Legacy confirmed slots cannot prove when or to whom they were raised]** → Preserve them as personal memory with synthetic origin IDs but do not fabricate party records.
- **[Older SkyrimNet builds expose dialogue timing but incomplete event payloads]** → Keep factual cue ownership and settle detection, use available source fields conservatively, and let state-aware expiry/fallback handle missing evidence without restoring periodic polling.

## Migration Plan

1. Introduce the ID/state data model, atomic transition API, versioned sidecar migration, and offline lifecycle/ledger tests while retaining no old runtime call path.
2. Add entry-owned floor grants, active-save generation isolation, and event/result ingestion; verify callbacks copy data, never call game or model APIs inline, and cannot cross a save transition.
3. Cut resolution over to the scheduler and batch prompt, then remove FormID+lens resolver requests, `lastCheckGameDays`, periodic polling, and the retired setting.
4. Add party-heard persistence, prompt context, cross-lens/party veto, Unicode keys, and matching pending-echo retirement.
5. Update SKSE Menu Framework, SkyUI MCM, prompt assets, settings help, documentation, and ADR in the same release.
6. Load a legacy sidecar in a targeted migration test, preserve its backup, verify rewritten state, and verify a new binary can continue after save/reload.
7. Exercise cue-owned raise, ignored/deferred/met outcomes, cross-follower suppression, long/short conversations, failed batches, time skip, and both user interfaces in-game before release.

Rollback: restore the pre-migration sidecar backup and prior DLL/scripts/prompt assets as one unit. Do not mix old scripts/prompts with the new DLL because decorator wording, batch schema, settings indices, and native UI calls change together.
