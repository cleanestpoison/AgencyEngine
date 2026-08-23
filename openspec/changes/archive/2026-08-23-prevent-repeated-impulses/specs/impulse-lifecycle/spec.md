## Purpose

Defines how a carried impulse keeps stable identity, owns a speaking opportunity, progresses after being raised, survives save/reload, expires, and appears consistently to users.

## ADDED Requirements

### Requirement: Persistent pending-entry identity
The system SHALL assign every carried impulse an opaque entry ID that is unique within its save and SHALL preserve that ID through cueing, resolution, ledger ownership, persistence, diagnostics, and user actions. Work naming an entry ID that is no longer current SHALL NOT mutate a replacement entry occupying the same companion and lens.

#### Scenario: Stale resolver result cannot clear a replacement
- **WHEN** a resolver request for entry 104 is in flight and entry 105 replaces it in the same companion and lens
- **THEN** a result naming entry 104 is ignored and entry 105 remains unchanged

#### Scenario: Identity survives reload
- **WHEN** a save is reloaded with an outstanding entry
- **THEN** the restored entry retains the same entry ID and lifecycle state

### Requirement: One entry owns each floor grant
The system SHALL bind every emitted coalesced cue to one exact eligible entry while keeping the visible cue free of subject details. The newest eligible carry before cue emission SHALL own the grant, ownership SHALL remain fixed until that floor closes, and other carried entries SHALL remain background context rather than additional granted subjects.

#### Scenario: Multiple entries coexist behind one cue
- **WHEN** Lydia carries entries 104 and 105 and the cue grants the floor to entry 105
- **THEN** only entry 105 is treated as explicitly requested while entry 104 remains carried as conversational background

#### Scenario: A later carry updates an unissued cue
- **WHEN** a newer eligible entry arrives before an existing coalesced cue is emitted
- **THEN** the cue grants the floor to the newer entry without creating an additional cue

### Requirement: Pending lifecycle is monotonic
Each entry SHALL have exactly one of `untouched`, `raised_unmet`, or `met` as its lifecycle outcome. Evidence that the subject was voiced SHALL move `untouched` to `raised_unmet`; evidence that the topic was answered or the proposal was actually completed or plainly refused SHALL move an open entry to `met`; and no result SHALL move an entry back to an earlier state. A proposal accepted for later but not performed SHALL remain `raised_unmet`.

#### Scenario: Ignored subject remains outstanding
- **WHEN** a companion raises an entry and the player gives no answer
- **THEN** the entry becomes `raised_unmet` and remains an outstanding thread

#### Scenario: Deferred proposal is not complete
- **WHEN** the player agrees to a proposed activity but the activity has not happened
- **THEN** the proposal remains `raised_unmet`

#### Scenario: Met entry closes
- **WHEN** reliable evidence establishes that an entry is met
- **THEN** the entry is removed from pending presentation and cannot be reopened by a later stale verdict

### Requirement: Entry-owned speech records a raise without model judgement
When a companion produces a qualifying dialogue event while owning an active floor grant, the system SHALL mark that exact entry `raised_unmet` without making an LLM request. If the owner produces no qualifying dialogue before the floor closes, the entry SHALL remain `untouched`.

#### Scenario: Cued companion speaks
- **WHEN** entry 104 owns Lydia’s active floor and Lydia produces a dialogue event during that floor
- **THEN** entry 104 becomes `raised_unmet` without a resolution-model call

#### Scenario: Cue expires silently
- **WHEN** an entry owns a cue but its companion never produces qualifying dialogue before the floor closes
- **THEN** the entry remains `untouched`

### Requirement: Expiry respects observed state
Expiry SHALL remove an `untouched` entry and withdraw only the provisional personal ledger record owned by that entry. Expiry of `raised_unmet` SHALL retire its stale pending presentation while preserving confirmed personal and party-heard memory. A `met` entry SHALL already be absent from pending presentation and its confirmed memory SHALL remain subject to normal ledger retention.

#### Scenario: Untouched entry expires
- **WHEN** an `untouched` entry reaches its pending TTL
- **THEN** the entry and its matching provisional ledger record are removed and the subject may become eligible again

#### Scenario: Raised entry expires
- **WHEN** a `raised_unmet` entry reaches its pending TTL
- **THEN** the outstanding bio thread is removed but confirmed anti-repeat memory is retained

### Requirement: Legacy pending data migrates without losing live state
When loading pending data written before stable IDs and three-state lifecycle, the system SHALL assign persistent IDs to live entries, map unspoken entries to `untouched`, map spoken-but-open entries to `raised_unmet`, preserve ledger confirmation, and rewrite the migrated representation on the next persistence sync.

#### Scenario: Old sidecar is loaded
- **WHEN** a sidecar contains a live legacy entry without an entry ID
- **THEN** the entry is restored with a new stable ID and the lifecycle state equivalent to its legacy fields

### Requirement: Both user interfaces expose the same lifecycle
SKSE Menu Framework and SkyUI MCM SHALL present the same live entries, entry states, identity diagnostics, expiry consequences, and available actions. Character-bio presentation SHALL distinguish an untouched subject to raise from a raised-but-unmet thread and SHALL NOT instruct a companion to introduce the latter as new.

#### Scenario: Raised entry is presented consistently
- **WHEN** an entry becomes `raised_unmet`
- **THEN** both interfaces and the character bio identify it as already raised and outstanding rather than unsaid

#### Scenario: User action targets one stable entry
- **WHEN** the user clears a specific pending row from either interface
- **THEN** the action applies to that row’s entry ID and cannot clear a replacement entry
