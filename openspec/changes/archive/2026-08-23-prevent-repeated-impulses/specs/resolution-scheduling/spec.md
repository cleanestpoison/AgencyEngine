## Purpose

Defines when pending impulses may consume paid semantic-resolution requests, how evidence is batched across followers, and how repeated or irrelevant events are prevented from causing model-call churn.

## ADDED Requirements

### Requirement: Event ingestion is model-free, asynchronous, and recoverable
Receiving a SkyrimNet dialogue or gameplay event SHALL NOT invoke an LLM request. The system SHALL copy callback data needed for later association while callback payloads are valid and SHALL also poll a bounded recent-event tail every 15 active real seconds so missed callbacks do not lose checkpoint progress. Callback and polled observations SHALL share source-ID deduplication. The first poll after each load SHALL establish a baseline without counting historical events. Every later accepted event SHALL receive a per-save monotonic ingestion sequence greater than every sequence previously assigned in that save. Any SkyrimNet source event ID SHALL be retained separately for deduplication and SHALL NOT serve as the persisted ordering watermark.

#### Scenario: Dialogue line arrives
- **WHEN** SkyrimNet invokes the dialogue callback for a follower line
- **THEN** AgencyEngine enqueues copied minimal evidence and returns without invoking a resolver or accessing game-thread-only state

#### Scenario: Callback payload expires
- **WHEN** SkyrimNet invalidates its callback string after return
- **THEN** later resolution uses AgencyEngine’s owned event data and never references the expired payload

#### Scenario: Callback delivery fails
- **WHEN** SkyrimNet records a new event but does not invoke AgencyEngine's registered callback
- **THEN** the next bounded recent-tail poll accepts that event once without invoking a resolver

#### Scenario: Callback and poll observe the same event
- **WHEN** an event arrives through the callback and remains present in the next recent-event tail
- **THEN** its SkyrimNet source ID prevents a second evidence sequence, floor transition, or checkpoint increment

#### Scenario: Polling begins after load
- **WHEN** AgencyEngine performs its first recent-tail poll for a newly active save
- **THEN** existing tail entries establish the source-ID baseline and do not count as new post-load evidence

#### Scenario: Game is paused or loading
- **WHEN** 15 real seconds pass while normal Director work is suspended
- **THEN** AgencyEngine performs no recent-tail query until active gameplay resumes

#### Scenario: New evidence follows a persisted watermark
- **WHEN** a save reloads after evidence sequence 220 was attempted and a genuinely new event then arrives
- **THEN** the event receives a per-save ingestion sequence greater than 220 regardless of its SkyrimNet source ID

### Requirement: Every open entry advances on accepted event checkpoints
Every verified `untouched` or `raised_unmet` entry SHALL become eligible after the configured number of newer accepted SkyrimNet events, regardless of whether local token matching considers those events relevant. The entry's checkpoint SHALL begin when it is carried and SHALL advance to the batch's upper evidence sequence whenever that entry is attempted. Event ingestion SHALL remain model-free; reaching a checkpoint makes semantic classification eligible but SHALL NOT itself determine the verdict.

#### Scenario: Untouched concern is settled indirectly
- **WHEN** Lydia carries an untouched request and 30 later events include an indirect action that makes the request unnecessary
- **THEN** the untouched entry appears in the next checkpoint batch so the semantic resolver can retire it without requiring Lydia to introduce it

#### Scenario: Unrelated activity reaches a checkpoint
- **WHEN** Lydia carries an untouched “a drink” entry and 30 later events do not address it
- **THEN** the entry is checked once and remains untouched when the resolver finds no resolution evidence

#### Scenario: Floor-owned raise remains state evidence
- **WHEN** Lydia's floor-owning line raises an entry during its current 30-event window
- **THEN** the entry remains `raised_unmet` without another model call at that moment and is classified at its next ordinary checkpoint

#### Scenario: Proposal gameplay signal is available at checkpoint
- **WHEN** a carried sparring proposal exists and later combat or activity evidence is observed
- **THEN** that evidence is available to the semantic resolver when the proposal reaches its checkpoint

### Requirement: Evidence watermarks prevent repeated paid checks
Each entry SHALL record the highest evidence sequence covered by its latest resolution attempt. The same evidence SHALL NOT independently trigger another paid request after success, failure, malformed output, conversation reopen, or save/reload. After an attempt, a later automatic attempt SHALL require the configured number of newer accepted events or the entry's one distinct pre-expiry fallback marker; older events MAY accompany the checkpoint as bounded context but SHALL NOT advance it.

#### Scenario: Same evidence is observed repeatedly
- **WHEN** a UI or scene transition exposes already-processed evidence more than once
- **THEN** source-ID deduplication and the persisted entry watermark prevent another checkpoint attempt

#### Scenario: Resolver failure does not spin
- **WHEN** a resolution request fails after consuming evidence through event 220
- **THEN** event 220 and earlier events cannot trigger an immediate retry, the entry remains unchanged, and another automatic attempt requires a full newer event interval or its pre-expiry fallback

#### Scenario: Watermark survives reload
- **WHEN** the game reloads after an entry was attempted through a particular event sequence
- **THEN** persisted scheduling state requires the configured count of genuinely new accepted events before another checkpoint

### Requirement: Entry-owned raises avoid immediate semantic calls
The system SHALL mark a floor-owned entry raised without model judgement. The raise SHALL NOT dispatch a semantic-resolution request or reset that entry's checkpoint. At its next checkpoint, bounded evidence after carry—including any later response or relevant action—SHALL allow the resolver to distinguish `met`, `raised_unmet`, and `untouched`.

#### Scenario: Player gives no response
- **WHEN** Lydia raises her floor-owned entry and no target response or relevant action follows before its checkpoint
- **THEN** the checkpoint classifies the evidence and leaves the entry `raised_unmet`

#### Scenario: Player responds ambiguously
- **WHEN** the player responds after Lydia raises her floor-owned proposal and the response cannot be classified by lifecycle facts alone
- **THEN** the response is available for semantic judgement at the entry's next checkpoint

### Requirement: Paid work batches every due open entry
At each automatic dispatch point, the system SHALL place every independently due verified entry across all followers and lenses into one resolver request. Shared chronological evidence SHALL appear once, and the resolver SHALL return one verdict per named entry ID. Entries that have not reached their own checkpoint SHALL remain absent and SHALL retain their watermark. Only one resolution batch SHALL be in flight; evidence arriving during it SHALL remain available for a later checkpoint.

#### Scenario: Several followers reach a checkpoint
- **WHEN** Lydia and Serana entries have each accumulated the configured number of newer events
- **THEN** one resolver request contains every due entry across both followers

#### Scenario: Newer entry is not yet due
- **WHEN** Kaidan's entry was carried after Lydia's and has accumulated fewer than the configured number of events
- **THEN** Kaidan's entry is absent from Lydia's checkpoint batch and keeps its existing watermark

### Requirement: Event interval and real-time cooldown bound automatic polling
An open entry SHALL become automatically due whenever 30 accepted events have sequences newer than its last-attempted watermark; the shipped interval SHALL default to 30. Automatic dispatch SHALL additionally require at least four real minutes since the previous automatic paid batch. Reaching either gate alone SHALL produce no request. Manual checks SHALL bypass both gates. Pre-expiry fallback requests SHALL bypass the event interval but remain subject to the automatic cooldown. Conversation silence and gameplay settlement SHALL NOT gate or reset checkpoint progress.

#### Scenario: Thirty events reach the first checkpoint
- **WHEN** an untouched entry has accumulated 29 accepted events since carry
- **THEN** no automatic request is dispatched
- **WHEN** the thirtieth accepted event arrives and the cooldown permits
- **THEN** one checkpoint batch is dispatched

#### Scenario: Busy world cannot postpone resolution
- **WHEN** unrelated dialogue and gameplay continue without a 100-second silent period
- **THEN** they advance open entries toward their checkpoints instead of resetting a global inactivity timer

#### Scenario: Event burst is rate limited
- **WHEN** another 30 events arrive less than four real minutes after the previous automatic batch
- **THEN** the due entries wait without losing evidence until the cooldown expires

#### Scenario: Manual check bypasses scheduling gates
- **WHEN** the user requests a paid check before either 30 events or four minutes have elapsed
- **THEN** the selected entry is dispatched immediately without duplicating queued or in-flight work

### Requirement: Resolver results apply independently by stable ID
A batch response SHALL classify entries as `untouched`, `raised_unmet`, or `met` and SHALL name the stable entry ID for every verdict. Valid current verdicts SHALL apply independently; missing, duplicated, unknown, malformed, stale, or regressive verdicts SHALL leave the affected entry unchanged and SHALL NOT prevent other valid verdicts from applying.

#### Scenario: Partial batch response
- **WHEN** a response contains one valid current verdict, one unknown ID, and omits a third requested ID
- **THEN** only the valid current verdict is applied and the other entries remain unchanged

#### Scenario: Older state is returned
- **WHEN** a response returns `untouched` for an entry already known to be `raised_unmet`
- **THEN** the regressive verdict is ignored

### Requirement: Asynchronous work is isolated by active save generation
Every queued event, eligible entry, in-flight batch, and resolver callback SHALL be bound to an active-save token containing the SkyrimNet save ID and a process-local generation that increments on every new-game or load transition, including a transition back to the same save. A callback SHALL apply verdicts or clear in-flight state only when both its save token and batch ID match the current in-flight batch. Changing the active-save token SHALL discard older queued work and SHALL NOT allow an older callback to mutate or unblock the new save.

#### Scenario: Old save callback collides with the same entry ID
- **WHEN** save A dispatches a batch for entry 104, save B loads with its own entry 104, and save A’s callback then returns
- **THEN** save B’s entry and current in-flight state remain unchanged

#### Scenario: Returning to the original save still changes generation
- **WHEN** a batch from save A remains outstanding while the player loads save B and then reloads save A
- **THEN** the old save A callback is rejected because its generation differs from the current save A generation

#### Scenario: Queued event belongs to the prior save
- **WHEN** a raw event stamped under save A remains queued when save B becomes active
- **THEN** the event is discarded before evidence sequencing or entry association in save B

### Requirement: Raised proposals receive at most one pre-expiry fallback
A still-open raised proposal that has not received decisive newer evidence SHALL receive at most one synthetic pre-expiry eligibility marker. Entries due together SHALL share one fallback batch. Expiry SHALL wait for an already-dispatched fallback result, but request failure or invalid output SHALL release that wait and apply normal state-aware expiry rather than polling again.

#### Scenario: Several proposals approach expiry
- **WHEN** three raised proposals become due for their first fallback together
- **THEN** one batch checks all three and each records that its fallback was consumed

#### Scenario: Fallback fails
- **WHEN** the pre-expiry resolver request fails
- **THEN** no repeated fallback is issued and each entry proceeds through state-aware expiry

### Requirement: Shipped settings express checkpoint polling
The recurring game-time `pendingResolveGameMinutes` control and the conversation-settled resolver dependency SHALL remain absent from saved output and both user interfaces. Matching resolution controls SHALL expose an accepted-event interval with a shipped default of 30 and an automatic real-time cooldown with a shipped default of 240 seconds. Help text SHALL explain that both gates must pass, activity advances rather than postpones the checkpoint, and manual checks bypass them. Legacy configurations containing retired resolver keys SHALL load without failing and SHALL emit obsolete-setting diagnostics rather than restoring old scheduling behavior. Optional private-thought generation SHALL remain user-selectable but SHALL default to off.

#### Scenario: Existing configuration contains timer setting
- **WHEN** settings load a legacy `pendingResolveGameMinutes` or `pendingResolveEventCap` value
- **THEN** neither value restores its old behavior, an obsolete-setting diagnostic is available, and subsequent saves omit the retired key

#### Scenario: User inspects checkpoint settings
- **WHEN** either user interface displays resolution controls
- **THEN** both show the same event interval and real-time cooldown and explain their conjunctive automatic-dispatch behavior

#### Scenario: Fresh install uses cost-safe thought default
- **WHEN** no explicit private-thought override exists
- **THEN** carrying an impulse does not issue a private-thought generation request

### Requirement: Resolution diagnostics are equivalent across interfaces
SKSE Menu Framework and SkyUI MCM SHALL expose matching live counts and details for eligible entries, queued evidence, batch-in-flight state, last trigger, evidence watermark, calls attempted, and entries classified. Diagnostics SHALL distinguish zero-call factual transitions from paid semantic batches.

#### Scenario: Cued raise needs no model
- **WHEN** an entry changes to `raised_unmet` from entry-owned speech without a resolver call
- **THEN** both interfaces show the state transition without incrementing the paid-batch counter
