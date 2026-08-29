## Purpose

Defines how impulse asks derive subjects and interpersonal meaning from actor state without prescribing a repetitive social repertoire, and how shipped context and forcing defaults support that judgment.

## ADDED Requirements

### Requirement: Lens prompts define boundaries without supplying a repertoire
The shipped impulse prompts SHALL define the lens purpose, admissible evidence, exclusions, and output contract without enumerating a stock catalog of activities, emotions, grievances, or dramatic situations for the model to complete. Content-bearing examples SHALL NOT repeatedly seed a subject or advertise that subject as desirable in another lens.

#### Scenario: Activity ask is rendered
- **WHEN** the shipped Activity lens is rendered for an ask
- **THEN** the rendered instructions define a voluntary shared activity or experience as the lens boundary without listing competitive, combative, recreational, romantic, or domestic subjects to choose from

#### Scenario: Relationship ask is rendered
- **WHEN** the shipped Relationship lens is rendered for an ask
- **THEN** the rendered instructions define presently relevant interpersonal standing as the lens boundary without listing positive or negative attitudes, grievances, admissions, debts, or confrontation patterns to choose from

#### Scenario: Another lens rejects out-of-scope material
- **WHEN** a shipped lens explains that a subject belongs to a different lens
- **THEN** it identifies the semantic boundary without including a detailed example that primes the rejected subject

### Requirement: Interpersonal interpretation is evidence-led
An impulse SHALL derive its subject, emotional register, and interpretation of another person's motives from the supplied actor state and stamped evidence. An observed action alone SHALL NOT be treated as evidence of a hostile, vain, affectionate, competitive, or otherwise inferred motive.

#### Scenario: Action is present without motive evidence
- **WHEN** the state records that one companion repeatedly performed an action but does not record how the prospective speaker interprets it
- **THEN** the generated impulse does not assign a motive or turn that action into interpersonal conflict

#### Scenario: Character state supports a sharp interpretation
- **WHEN** the speaker's personality, standing, thoughts, or stamped events support a sharp or hostile interpretation
- **THEN** the lens permits that register without softening it into mandatory warmth

#### Scenario: Character state supports warmth or restraint
- **WHEN** the speaker's state supports warmth, neutrality, or choosing not to raise anything
- **THEN** the lens does not sharpen the result merely to create drama

### Requirement: Silence is the shipped default for every ask
The shipped forced-impulse chance SHALL be 0 percent. When no explicit user override exists, every lens ask SHALL retain the silence response; an explicit configured value from 0 through 100 SHALL continue to control forcing.

#### Scenario: Existing install has no forced-chance deviation
- **WHEN** settings are loaded without an explicit forced-impulse chance
- **THEN** the effective forced-impulse chance is 0 and the rendered ask offers silence

#### Scenario: User explicitly enables forcing
- **WHEN** settings contain an explicit forced-impulse chance greater than 0
- **THEN** the configured value is preserved and continues to govern the per-ask forced roll

### Requirement: Generation receives a seventy-event default window
The shipped recent-event budget SHALL be 70 events. When no explicit user override exists, an impulse ask SHALL request up to 70 recent events before applying its existing filtering and per-follower limits; an explicit configured event budget SHALL remain authoritative.

#### Scenario: Existing install has no event-budget deviation
- **WHEN** settings are loaded without an explicit recent-event budget
- **THEN** the effective event budget is 70

#### Scenario: User has tuned the event budget
- **WHEN** settings contain an explicit recent-event budget different from 70
- **THEN** the configured value is preserved and used for subsequent asks

### Requirement: Both configuration interfaces describe the same defaults
SKSE Menu Framework and SkyUI MCM SHALL expose the same effective forced-impulse chance and recent-event budget, SHALL identify 0 percent and 70 events as the shipped defaults, and SHALL provide equivalent guidance about silence and event context using presentation appropriate to each interface.

#### Scenario: User compares both interfaces
- **WHEN** the user opens the impulse settings in SKSE Menu Framework and SkyUI MCM on the same installation
- **THEN** both surfaces show the same current values, shipped-default descriptions, valid ranges, and behavioral meaning for forcing and event budget
