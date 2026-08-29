## Why

The shipped Activity and Relationship prompts repeatedly demonstrate competitive and confrontational subjects, causing models to reinterpret ordinary party behavior as rivalry, sparring challenges, or grievances instead of deriving an actor-specific impulse from the supplied state. Independent forced asks amplify that bias, while the current event budget can omit older evidence that would support more grounded and varied choices.

## What Changes

- Rewrite and cut the rendered Activity and Relationship instructions so they define eligibility, evidence, and ownership boundaries without supplying a catalog of activities, emotions, or dramatic situations.
- Remove content-bearing examples that seed sparring, combat-seeking, wagering, grandstanding, suspicion, or similar subjects, including cross-lens examples that advertise those subjects as desirable elsewhere.
- Tighten the evidence rule so the state must support not only an observed action but also any motive or interpersonal interpretation assigned to it.
- Preserve character-authentic hostility, competitiveness, warmth, and restraint when supported by the actor state; do not replace negative priming with mandatory positivity.
- Change the shipped `forcedImpulseChance` default from 20 to 0 so silence remains available on every default ask.
- Change the shipped `maxEvents` default from 40 to 70 so generation can consider a broader recent-event window.
- Update both SKSE Menu Framework and SkyUI MCM presentations, configuration examples, tests, and user-facing documentation to describe the new defaults and prompt behavior consistently.

## Capabilities

### New Capabilities
- `impulse-generation`: Defines evidence-led impulse selection, prompt neutrality, silence behavior, event context, and matching configuration presentation across both user interfaces.

### Modified Capabilities

None.

## Impact

- Prompt assets: the shared impulse base and the Activity, Relationship, and Aspiration lens overrides where loaded examples or repeated subject catalogs currently appear.
- Defaults and persistence: `Settings`, shipped configuration examples, deviation-only save behavior, and tests that assert shipped values.
- UI parity: SKSE Menu Framework and SkyUI MCM labels, help text, and default descriptions for forced asks and event budget.
- Documentation: prompt inventory, tuning guidance, and any design text that still describes a 20% shipped forced chance or a 40-event budget.
- No configuration migration is required: deviation-only persistence continues to preserve explicit user overrides while absent keys adopt the new shipped defaults.
