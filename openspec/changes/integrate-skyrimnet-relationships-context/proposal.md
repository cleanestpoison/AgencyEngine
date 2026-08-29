## Why

AgencyEngine lens asks currently receive SeverActions standing but cannot see the richer player-bond, consent-boundary, and preference state maintained by SkyrimNet Relationships. As a result, a companion's ordinary dialogue can be bond-aware while AgencyEngine decides what they should raise from thinner or contradictory relationship context.

## What Changes

- Add optional integration with SkyrimNet Relationships 1.1.2 or newer for every AgencyEngine lens ask.
- For an enrolled companion, replace the SeverActions player-standing passage with a concise, factual summary of the companion's Relationships bond state, while retaining SeverActions companion-to-companion opinions.
- Include qualitative bond kind and depth, player stance, romantic and physical availability, permanent versus present physical boundaries, authored disposition reason and fixed limit, established orientation, and authored likes/dislikes when those facts exist.
- Read all 58 Romantasy preference factions in one prompt pass and render only established likes or dislikes.
- Fall back to the existing SeverActions player blurb when SkyrimNet Relationships is unavailable or the companion is not enrolled. Do not merge missing Relationships fields with SeverActions prose.
- Keep Relationships state read-only: AgencyEngine does not initiate or record spark, consent, bond, or progression transitions.
- Add no setting or user-interface surface; integration activates automatically when its supported dependency and enrolled state are present.
- Preserve all existing lens cadence, forced-turn, impulse lifecycle, resolution, and delivery behavior.

## Capabilities

### New Capabilities
- `external-standing-context`: Supplies authoritative optional player-standing and preference context to all lens asks while preserving source ownership, fallback behavior, and relationship boundaries.

### Modified Capabilities

None.

## Impact

- Prompt content under `statics/SKSE/Plugins/SkyrimNet/prompts/`, principally the shared impulse base plus lens-specific constraints where needed.
- Runtime decorator fallback registration in `plugin.cpp` so prompts remain parseable when SkyrimNet Relationships is absent.
- Optional interoperability with `SNRom_Integration.esl` version 1.1.2 or newer; no bundled external files and no hard dependency for AgencyEngine users.
- Documentation describing the optional integration, source precedence, supported version, and in-game verification expectations.
- No SKSE Menu Framework or SkyUI MCM changes.