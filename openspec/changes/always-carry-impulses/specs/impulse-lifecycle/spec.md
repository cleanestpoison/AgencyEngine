## ADDED Requirements

### Requirement: Every accepted impulse is carried
The system SHALL create a pending carried entry for every accepted impulse and SHALL make that entry available only in its speaker's private character-bio prompt contexts. Carrying SHALL occur before any optional private-thought request or cue, and users SHALL NOT be able to select a thought-only delivery mode.

#### Scenario: Default delivery without an extra thought
- **WHEN** an impulse is accepted while optional private-thought generation is disabled
- **THEN** the system creates the pending carried entry without requesting a private thought

#### Scenario: Optional thought follows carrying
- **WHEN** an impulse is accepted while optional private-thought generation is enabled
- **THEN** the system creates the pending carried entry before requesting the additional private thought

#### Scenario: Cueing remains independently configurable
- **WHEN** an impulse is carried while cueing is disabled
- **THEN** the pending entry remains available as private conversational background without generating a cue or substituting a private thought

#### Scenario: Obsolete delivery override is present
- **WHEN** an existing configuration contains the removed bio-injection setting with any value
- **THEN** the system ignores that value and carries every accepted impulse
