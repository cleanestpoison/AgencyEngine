## Purpose

Provide every AgencyEngine lens with one coherent, optional account of a companion's standing with the player, including bond boundaries and personal preferences, without transferring ownership of the external relationship state.

## ADDED Requirements

### Requirement: Supported Relationships state enriches every lens ask
AgencyEngine SHALL include external standing context for each enrolled companion in every Aspiration, Relationship, Activity, and Curiosity lens ask when SkyrimNet Relationships 1.1.2 or newer is available.

#### Scenario: Enrolled companion appears in every lens context
- **WHEN** a supported SkyrimNet Relationships installation reports a companion as enrolled
- **THEN** each of the four lens asks includes that companion's Relationships-derived player-standing summary

#### Scenario: Integration is automatic
- **WHEN** the supported integration and an enrolled companion are present
- **THEN** AgencyEngine includes the external standing context without requiring a new setting or user-interface action

### Requirement: Enrolled Relationships standing is authoritative for the player
For an enrolled companion, AgencyEngine SHALL use SkyrimNet Relationships as the sole source of that companion's standing with the player. AgencyEngine SHALL NOT supplement missing Relationships facets with SeverActions player prose, and SHALL continue to include SeverActions companion-to-companion opinions independently.

#### Scenario: Both relationship systems provide player standing
- **WHEN** an enrolled companion has both Relationships bond state and a SeverActions player blurb
- **THEN** the lens context includes the Relationships player-standing summary and omits the SeverActions player blurb

#### Scenario: Enrolled state is partial
- **WHEN** an enrolled companion has bond state but one or more optional authored facets are absent
- **THEN** the lens context omits the absent facets and does not fill them from SeverActions

#### Scenario: Companion opinions coexist
- **WHEN** an enrolled companion also has SeverActions opinions about other companions
- **THEN** the lens context includes those companion-to-companion opinions alongside the Relationships player-standing summary

### Requirement: Existing player-standing fallback remains available
AgencyEngine SHALL use the existing SeverActions player-standing behavior when SkyrimNet Relationships is unavailable, unsupported, or reports the companion as unenrolled. The optional integration SHALL NOT make any AgencyEngine lens unusable when Relationships is absent.

#### Scenario: Relationships is absent
- **WHEN** SkyrimNet Relationships is not installed
- **THEN** every lens remains renderable and uses the existing SeverActions player-standing fallback

#### Scenario: Companion is unenrolled
- **WHEN** SkyrimNet Relationships is available but reports a companion as unenrolled
- **THEN** that companion's lens context uses the existing SeverActions player-standing fallback

#### Scenario: Neither source has player standing
- **WHEN** a companion is unenrolled and SeverActions provides no player blurb
- **THEN** the existing neutral fallback passage remains unchanged

### Requirement: Bond state is expressed as domain facts
The Relationships summary SHALL express bond kind and depth as concise qualitative facts. It SHALL preserve separate platonic and romantic ladders across all six depths and SHALL preserve accepted, declined, unanswered, and unavailable romantic states where applicable. It SHALL NOT expose points, numeric tiers, implementation level names, raw disposition values, distance to the next threshold, or form of address.

#### Scenario: Deep platonic bond
- **WHEN** an enrolled companion is at the highest bond depth without a romantic spark
- **THEN** the summary describes a permanent platonic life bond without calling the companion a spouse or exposing a tier number

#### Scenario: Romantic stance is unanswered
- **WHEN** an enrolled companion has a romantic spark and the player's stance is unanswered
- **THEN** the summary states that romantic feeling exists without mutuality or claim and does not stage the unanswered transition

#### Scenario: Player declined romance
- **WHEN** the player's recorded stance is declined
- **THEN** the summary preserves the existing feeling and chosen friendship without inviting AgencyEngine to reopen the question

### Requirement: Decision-relevant boundaries remain distinct
The summary SHALL include romantic availability, current physical availability, permanent physical unavailability, authored fixed limits, and established orientation only when each fact is known. Physical availability SHALL remain independent from romantic commitment. A fixed limit SHALL remain authoritative regardless of bond depth or availability.

#### Scenario: Physical but not romantic availability
- **WHEN** a companion is physically available but has no romantic relationship with the player
- **THEN** Activity may treat physical closeness as licensed but not required, while no lens treats physical availability as romantic commitment

#### Scenario: Physical availability is temporary
- **WHEN** the companion is not physically available at the current bond depth but may become available later
- **THEN** the summary describes present unavailability rather than a permanent refusal

#### Scenario: Physical boundary is permanent
- **WHEN** the companion's authored physical threshold marks physical intimacy as never available
- **THEN** the summary describes a permanent boundary and does not imply that waiting or progression can change it

#### Scenario: Fixed limit conflicts with availability
- **WHEN** another standing fact would otherwise license conduct forbidden by the companion's authored fixed limit
- **THEN** the fixed limit remains the controlling boundary in the lens context

### Requirement: Every established preference is available as personal standing
For an enrolled companion, AgencyEngine SHALL include every established like and dislike across the complete 58-activity Romantasy preference vocabulary. It SHALL omit activities for which the companion has no opinion and SHALL present preferences as that companion's personal leanings rather than global rules or morality.

#### Scenario: Mixed authored preferences
- **WHEN** a companion likes persuasion and murder, dislikes assault, and has no opinion about lockpicking
- **THEN** the summary includes persuasion and murder as likes, assault as a dislike, and no statement about lockpicking

#### Scenario: No preferences are authored
- **WHEN** an enrolled companion has no established likes or dislikes
- **THEN** the summary omits preference facts rather than describing the companion as neutral

### Requirement: External state remains read-only lens context
AgencyEngine SHALL use Relationships data only to inform existing lens decisions. AgencyEngine SHALL NOT initiate, record, or imply completion of a Relationships spark, consent, bond-kind, tier, or progression transition. Existing AgencyEngine cadence, forced-turn, impulse lifecycle, resolution, and delivery behavior SHALL remain unchanged.

#### Scenario: Relationship lens sees an unanswered stance
- **WHEN** the Relationship lens evaluates a companion whose romantic stance remains unanswered
- **THEN** it does not create the Relationships-owned consent transition or claim that the state changed

#### Scenario: Existing impulse behavior is unchanged
- **WHEN** external standing context is present during an ask
- **THEN** ask scheduling, forced-turn selection, carrying, cueing, resolution, and retirement follow the same rules as an ask without that context
