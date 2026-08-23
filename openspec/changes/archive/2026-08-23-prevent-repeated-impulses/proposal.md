## Why

Followers can present a carried subject as unsaid after already raising it, repeat topics across lenses or companions, and spend paid resolver calls while no relevant evidence exists. The underlying problem is lifecycle and memory: AgencyEngine conflates “never raised” with “raised but unresolved,” attributes cues and callbacks to mutable slots instead of stable entries, and polls each entry independently.

## What Changes

- Give every pending impulse a persistent entry ID and bind its cue, resolver work, ledger ownership, persistence, diagnostics, and UI actions to that ID.
- Scope every queued event and asynchronous resolver batch to the active SkyrimNet save ID plus a load generation, so a callback from a previous save cannot collide with an identical entry ID or clear the new save’s in-flight state.
- Replace the carried/spoken boolean with monotonic `untouched`, `raised_unmet`, and `met` states. An observed raise confirms anti-repeat memory even when the player ignores, refuses, or defers the subject.
- Bind each coalesced floor grant to one exact entry while leaving other carried entries available only as background conversational drift.
- Expire untouched and raised entries differently: withdraw an untouched entry’s provisional personal slot, but retain confirmed memory when retiring a raised thread.
- Add a bounded, recent party-heard ledger alongside durable per-companion lens ledgers so another follower does not immediately introduce the same subject. Preserve the original speaker and target for diagnostics and matching.
- Make local exact suppression cross-lens and party-wide, Unicode-safe, and unable to replace a confirmed slot with a provisional duplicate. Semantic duplicate classification remains out of scope until play evidence shows exact and prompt-based suppression are insufficient.
- Replace recurring game-time resolution polling with persisted accepted-event checkpoints and cross-follower batches. Dialogue and gameplay callbacks only enqueue copied event data. Every open untouched or raised/unmet entry becomes due after 30 newer events, so activity advances rather than postpones resolution and unspoken concerns can become moot. Per-entry watermarks prevent the same interval from retriggering paid calls, while a four-real-minute global cooldown bounds automatic cost.
- Mark an entry-specific cued raise without an LLM call; use at most one shared resolver request for all eligible entries that need semantic judgement. Default optional private-thought generation to off so the new resolver does not increase shipped LLM cost.
- Present matching lifecycle, party-memory, scheduling, diagnostics, settings, and actions in SKSE Menu Framework and SkyUI MCM.
- **BREAKING**: retire `pendingResolveGameMinutes` and `pendingResolveEventCap`; replace them with an accepted-event interval and automatic real-time cooldown whose meanings are explicit in saved settings and both user interfaces.

## Capabilities

### New Capabilities

- `impulse-lifecycle`: Stable pending-entry identity, entry-owned floor grants, monotonic raise/resolution states, persistence, expiry, presentation, and actions.
- `topic-memory`: Personal provisional/confirmed ledger behavior, recent party-heard memory, cross-lens and cross-follower suppression, retention, and Unicode-safe topic identity.
- `resolution-scheduling`: Cost-bounded event ingestion, per-entry evidence watermarks, 30-event cross-follower checkpoints, global automatic cooldown, and pre-expiry fallback.

### Modified Capabilities

None. This repository did not previously contain OpenSpec capability specifications.

## Impact

- Core state and persistence: `include/PendingImpulse.h`, `src/PendingImpulse.cpp`, and the pending sidecar format, including migration of existing entries and ledger slots.
- Scheduling and integration: `src/Director.cpp`, `src/SkyrimNetAPI.cpp`, related headers/state, dialogue event callbacks, cue ownership, and resolver prompt/response contracts.
- Configuration and presentation: `include/Settings.h`, `src/Settings.cpp`, `src/UI.cpp`, `Source/Scripts/AgencyEngine_MCM.psc`, and both interfaces’ labels, help, diagnostics, and actions.
- Prompt assets and documentation: pending-impulse bio decorators, resolver prompts, shipped prompt copies, `README.md`, `CONTEXT.md`, specifications, and an ADR for the new lifecycle and scheduling policy.
- Verification: state/ledger/settings tests, scheduler and stale-callback tests, prompt-contract checks, targeted plugin smoke checks, and in-game exercise of both SKSE Menu Framework and SkyUI MCM.
