## Context

See `proposal.md` for motivation. The delivery function currently chooses between a pending carried entry and a legacy private-thought-only fallback using `pendingBioInjection`. The same boolean is persisted as a configuration deviation and exposed independently by SKSE Menu Framework and SkyUI MCM. Cueing already depends on a successfully created pending entry, while `generateThought` independently controls the optional second thought request only on the carry path.

## Goals / Non-Goals

**Goals:**
- Make successful pending-entry creation a prerequisite for every accepted impulse's downstream delivery work.
- Preserve independent cue and optional-thought controls.
- Remove the obsolete preference cleanly from runtime state, persistence, diagnostics, documentation, and both interfaces.
- Treat old configuration values as inert without a compatibility mode.

**Non-Goals:**
- Change how pending entries are rendered, coalesced, resolved, expired, or persisted.
- Retune lens cadence, forced-impulse probability, cue timing, or SkyrimNet's own autonomous thought behavior.
- Migrate old thought-only outputs into pending entries.

## Decisions

### Carry first through one delivery path

`CarryImpulse` will always attempt pending-entry creation. Optional thought generation will run only after that creation succeeds, and cue creation will continue to run only after success and when enabled. If carrying fails, the impulse remains a failed delivery; it will not silently substitute a generated thought.

Alternative: retain the fallback only for technical carry failures. Rejected because it would report a qualitatively different behavior as successful and hide the failure of the core delivery invariant.

### Remove rather than deprecate the setting

The setting field, load/save handling, UI controls, help text, diagnostics, and example key will be removed in one cutover. Existing JSON files require no file migration: the settings loader reads known keys individually, so an obsolete key is naturally ignored, and later settings saves omit it.

Alternative: preserve a deprecated hidden field. Rejected because it retains two delivery semantics and makes future behavior depend on stale configuration.

### Preserve the meaning of the remaining controls

`generateThought` means exactly one optional second LLM call after a successful carry. `cues` means whether a successful fresh carry receives a speaking opportunity. Neither setting changes whether the impulse is carried.

### Remove the interface row in parity

SKSE Menu Framework and SkyUI MCM will both remove the setting in the same change. SkyUI MCM's index-based boolean arrays and all corresponding callbacks/help dispatch must be compacted consistently so later settings do not shift onto the wrong keys.

## Risks / Trade-offs

- [Existing users intentionally selected the experimental thought-only A/B] → The breaking change is explicit; carrying is restored unconditionally and the stale key is ignored.
- [Removing one MCM boolean index misroutes later controls] → Update key/default/OID arrays and every index reference together, then exercise each surviving boolean in tests and both in-game interfaces.
- [Carry failure previously produced some output through the fallback] → Fail loudly instead; a private thought is not an acceptable substitute for the delivery contract.
- [SkyrimNet's autonomous thoughts may still mention carried impulses] → This change does not alter `render_mode == "thoughts"`; the removed option governs only AgencyEngine's explicit second thought call.

## Migration Plan

1. Ship code and both interfaces without the setting.
2. On load, old `pendingBioInjection` keys are ignored because no runtime field reads them.
3. On the next settings save, deviation-only serialization omits the obsolete key.
4. Rollback restores the prior binary and interfaces; an old configuration that still contains the key resumes its prior behavior.
