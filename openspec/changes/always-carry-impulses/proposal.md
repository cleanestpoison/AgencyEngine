## Why

Disabling `pendingBioInjection` replaces AgencyEngine's core carry-and-cue delivery with a private-thought-only fallback, so the mod can report successful impulses that never enter dialogue. Carrying is the delivery invariant and should not be exposed as an optional preference.

## What Changes

- Every accepted impulse is stored as a pending entry and rendered privately in its speaker's character bio.
- Remove the thought-only delivery fallback and the `pendingBioInjection` setting from runtime configuration and both user interfaces.
- Keep `generateThought` as the sole control for the optional second private-thought LLM call.
- Keep `cues` as the independent control for granting a fresh carried impulse a speaking opportunity.
- **BREAKING**: Existing `pendingBioInjection` configuration overrides are obsolete and no longer affect behavior.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `impulse-lifecycle`: Require every accepted impulse to enter the pending carried lifecycle before optional thought generation or cueing.

## Impact

- Delivery flow in `src/Director.cpp` and the `Settings` model and persistence code.
- SKSE Menu Framework and SkyUI MCM configuration surfaces, which must remove the setting in parity.
- Shipped configuration example, user documentation, and settings tests.
- Existing configuration files may retain an ignored obsolete key until another save rewrites deviations.
