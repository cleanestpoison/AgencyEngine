# Stable impulses use owned speech and evidence-driven resolution

## Decision

A pending impulse is a persisted entry with a monotonic `EntryId`, speaker identity, target scope, origin-owned memory, and one of three states:

- **Untouched** — carried but not observed aloud.
- **Raised/unmet** — introduced, but not answered or completed.
- **Met** — resolved and retired.

Commands and asynchronous work name the entry ID, never a mutable companion/lens slot. State transitions are monotonic and execute with their personal and party-memory changes under the pending-store lock.

A coalesced cue owns the newest entry before emission. Its 30-second floor grant then becomes immutable. Qualifying speech from that exact speaker, received after the grant, raises that exact entry without an LLM call. The owner line remains lifecycle evidence but does not itself dispatch resolution; later dialogue and gameplay remain available to the entry's ordinary event checkpoint.

Confirmed memory has two layers. Personal exact memory suppresses the same companion across every lens while each lens keeps its own bounded insertion ring. Party-heard exact memory suppresses cross-follower echoes for seven game days. Both records retain their origin entry ID so stop and forget actions remove only what they own.

Resolution advances through accepted-event checkpoints. SkyrimNet callbacks provide the low-latency observation path, while a bounded `PublicGetRecentEvents` tail poll every 15 active real seconds recovers callbacks that were not delivered. The first poll after each load establishes a non-counting baseline; callbacks and polls share source-ID deduplication. The Director assigns every later accepted event a persisted per-save evidence sequence. Every verified open entry—untouched or raised/unmet—is due after 30 newer accepted events, so an unspoken concern can become moot and a spoken concern can be answered without depending on literal topic matching. Independently due entries across followers share one batch. Automatic batches are globally rate-limited to one every four real minutes; unrelated activity advances checkpoints instead of refreshing a global silence gate. Manual requests bypass both gates, while a raised proposal retains one pre-expiry fallback. Each entry persists its last-attempted watermark, so failed or malformed requests cannot spin on the same evidence.

Every raw event, batch, and callback result carries `ActiveSaveToken { saveId, generation }`; batches also carry a batch ID. Generation changes on every load/new game, including A→B→A. Only the Director thread applies a complete token match. A stale callback cannot mutate an entry or clear another save's in-flight batch.

## Consequences

- `pendingResolveGameMinutes` and `pendingResolveEventCap` are retired. `pendingResolveEventInterval` defaults to 30 accepted events and `pendingResolveCooldownSeconds` defaults to 240 real seconds.
- Private-thought generation remains optional and defaults off. A normal carry costs only its selection call; an observed floor-owned raise costs zero additional calls.
- The resolver prompt receives bounded recent SkyrimNet history, shared chronological callback events, and ID-keyed entries, then returns independent tri-state verdicts. Missing, duplicate, unknown, stale, malformed, and regressive verdicts are ignored.
- Untouched expiry withdraws provisional personal memory. Raised/unmet expiry preserves confirmed personal and party memory. A dispatched pre-expiry fallback protects its entries until its result or failure is consumed.
- SKSE Menu Framework and SkyUI MCM expose the same stable IDs, lifecycle, two memory layers, scheduler and recovery-poll diagnostics, event interval, real-time cooldown, paid manual actions, and entry-owned stop/forget actions.

## ADR 0004 resolution

ADR 0004 left cue attribution and voiced-but-unmet behavior open. Entry-owned floor grants now provide exact attribution without putting the topic in the cue. Raised/unmet is a first-class durable state, so an ignored raise remains open but is never presented as unsaid or instructed to introduce itself again. This preserves ADR 0004's vague coalesced cue and ADR 0003's independent per-lens cadence and memory rings.

## Rejected alternatives

- **FormID plus lens identity.** A stale callback can hit a replacement occupying that slot.
- **Per-entry periodic polling.** Cost grows with open entries and repeats calls without new evidence.
- **Read SkyrimNet's private database.** It would couple AgencyEngine to internal schema and locking when the public bounded-tail interface already supplies stable source IDs and event payloads.
- **Treat the owner's opening line as resolution evidence.** It spends a call before anyone responds and can immediately classify the raise itself.
- **One global topic ledger.** It erases legitimate companion-specific perspective and breaks per-lens eviction.
