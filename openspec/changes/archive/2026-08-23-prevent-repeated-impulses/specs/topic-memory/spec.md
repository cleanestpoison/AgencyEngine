## Purpose

Defines durable personal and recent party-wide memory of raised subjects so carried impulses remain novel without erasing legitimate companion-specific perspectives forever.

## ADDED Requirements

### Requirement: Personal memory distinguishes provisional carry from confirmed raise
Carrying a topic SHALL create or retain a provisional personal ledger record owned by that entry and lens. Observing the topic raised SHALL confirm that personal record even if the subject remains unmet. Confirmation SHALL be monotonic: a provisional observation SHALL NOT replace, relocate, or weaken an existing confirmed record for the same normalized subject.

#### Scenario: Carry reserves a personal subject
- **WHEN** Lydia begins carrying an untouched Activity topic named “a drink”
- **THEN** Lydia’s Activity ledger contains a provisional record that suppresses another personal carry of that subject

#### Scenario: Raise confirms memory without an answer
- **WHEN** Lydia raises “a drink” and the player ignores her
- **THEN** Lydia’s personal record becomes confirmed while the pending entry remains `raised_unmet`

#### Scenario: Provisional duplicate cannot downgrade confirmation
- **WHEN** a provisional record attempt matches a confirmed personal record in another lens
- **THEN** the confirmed record remains in its owning ring and no provisional replacement is inserted

### Requirement: Personal exact suppression spans every lens
A candidate topic SHALL be rejected when its normalized key matches any provisional or confirmed personal ledger record belonging to that companion, regardless of lens. Ledger capacity and eviction SHALL remain per lens so recording one lens SHALL NOT evict another lens’s subjects.

#### Scenario: Cross-lens duplicate is rejected
- **WHEN** Lydia’s Aspiration ledger contains confirmed topic “a bed” and Activity generates exact normalized topic “a bed”
- **THEN** the Activity candidate is rejected without changing the Aspiration record

#### Scenario: Eviction remains local to the owning lens
- **WHEN** Lydia’s Activity ring is full and receives a distinct Activity topic
- **THEN** only the oldest eligible Activity record is displaced and no Aspiration, Curiosity, or Relationship record is removed

### Requirement: Raised subjects enter recent party-heard memory
When a subject becomes `raised_unmet` or `met`, the system SHALL record it in a party-heard ledger with its entry ID, original speaker, target scope, original display topic, normalized topic key, and raise time. An untouched carry SHALL NOT enter party-heard memory.

#### Scenario: Another follower hears a raised subject
- **WHEN** Lydia raises “returning to Whiterun” to the player
- **THEN** party-heard memory records the subject as raised by Lydia to the player

#### Scenario: Private carry is not shared prematurely
- **WHEN** Lydia carries “returning to Whiterun” but never raises it
- **THEN** Serana is not suppressed solely by Lydia’s untouched carry

### Requirement: Party-heard memory suppresses recent cross-follower echoes
A candidate SHALL be rejected when its normalized topic and compatible target scope match a retained party-heard record, including when the candidate comes from another companion or another lens. Two known and unequal targets SHALL be treated as different scopes; an unknown target SHALL be party-wide. Recording a party-heard subject SHALL also retire exact-matching `untouched` carries owned by other companions while leaving already raised threads intact.

#### Scenario: Another follower repeats the same subject
- **WHEN** Lydia recently raised “a drink” to the player and Serana generates exact normalized topic “a drink” for the player
- **THEN** Serana’s candidate is rejected locally without an additional LLM call

#### Scenario: Existing untouched echo is retired
- **WHEN** Serana already carries an untouched “a drink” entry and Lydia then raises that subject to the same target scope
- **THEN** Serana’s matching entry and provisional personal record are withdrawn before it can receive a floor grant

#### Scenario: Different known targets retain distinct perspectives
- **WHEN** two exact topic strings refer to explicitly known and different targets
- **THEN** the first target’s party-heard record does not suppress the second target’s candidate

### Requirement: Party-heard retention is bounded and recent
Party-heard records SHALL expire after the configured recent-echo window, which SHALL default to seven game days, and the ledger SHALL retain at most 32 records by evicting the oldest record when capacity is exceeded. Local exact suppression SHALL consult every retained record, while generated prompt context SHALL include at most the 12 newest records to bound tokens. Both user interfaces SHALL expose the recent-echo window and current occupancy with identical semantics.

#### Scenario: Recent echo remains suppressed
- **WHEN** a party-heard subject is younger than seven game days and has not been displaced by capacity
- **THEN** an exact compatible candidate remains suppressed

#### Scenario: Old echo becomes eligible again
- **WHEN** a party-heard record exceeds the configured recent-echo window
- **THEN** it is removed from party-heard suppression without removing any companion’s durable personal record

#### Scenario: Prompt context remains bounded
- **WHEN** party-heard memory contains more than 12 retained records
- **THEN** local exact suppression checks all retained records but the generation prompt renders only the 12 newest

### Requirement: Topic keys are Unicode-safe and deterministic
Topic identity SHALL preserve valid Unicode letters and numbers, apply Unicode-aware compatibility normalization and case folding, treat punctuation and repeated whitespace as separators, and preserve the original topic for display. Canonically equivalent forms SHALL produce the same non-empty key, and a genuinely empty topic SHALL be rejected from ledger recording with an observable diagnostic rather than silently creating an unusable record.

#### Scenario: Non-Latin topic receives a key
- **WHEN** a valid topic is “доля монет” or “父親の剣”
- **THEN** normalization produces a non-empty key that participates in exact suppression

#### Scenario: Equivalent Unicode forms match
- **WHEN** two topics differ only by Unicode case or canonical or compatibility representation
- **THEN** they normalize to the same key

#### Scenario: Display text remains intact
- **WHEN** a normalized topic is rendered in either interface or a prompt
- **THEN** users and models see the original topic text rather than the normalized key

### Requirement: Forgetting distinguishes pending state from remembered history
Both user interfaces SHALL offer equivalent actions to stop carrying an entry while preserving confirmed history and to forget that entry’s subject memory deliberately. Forgetting subject memory SHALL remove personal and party-heard records owned by that entry ID; independent records from another entry SHALL remain. A global forget action SHALL clear pending entries and both memory layers.

#### Scenario: Stop carrying a raised thread
- **WHEN** the user stops carrying a `raised_unmet` entry
- **THEN** its pending presentation is removed and its confirmed personal and party-heard records remain

#### Scenario: Forget one subject deliberately
- **WHEN** the user chooses to forget a specific entry’s subject
- **THEN** that entry and its owned personal and party-heard records are removed without deleting another entry’s independent record

### Requirement: Semantic avoidance adds no dedicated classifier call
Generation context SHALL show the compact recent party-heard list and instruct the generation model to avoid semantically equivalent rewordings. Exact normalized suppression SHALL remain the local code backstop, and the system SHALL NOT make a separate LLM request solely to classify candidate-topic similarity.

#### Scenario: Reworded subject is presented for self-checking
- **WHEN** the recent party list contains “the Elder Scroll at Tower Mzark”
- **THEN** the impulse-generation prompt can compare a candidate such as “the College Elder Scroll leads” against that subject without another model request
