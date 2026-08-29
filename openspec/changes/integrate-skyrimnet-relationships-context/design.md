## Context

See `proposal.md` for motivation and `specs/external-standing-context/spec.md` for the behavior contract.

All lens prompts extend `agencyengine_impulse_base.prompt`, whose per-follower standing section currently calls the SeverActions `sever_player_blurb` and `sever_companion_opinions` decorators. AgencyEngine registers inert versions of those names when SeverActions is absent because Inja resolves decorator names while parsing, before a template guard can suppress a call.

SkyrimNet Relationships 1.1.2 exposes rich bond state through `get_romance` and current physical availability through `romance_physical_ok`. It does not expose an aggregate preferences decorator; its shipped preference prompt reads Romantasy's 58 opinion factions. The external license expressly permits interoperability but not redistribution of the mod or substantial parts of its prompt content.

## Goals / Non-Goals

**Goals:**
- Make one Relationships-derived player-standing summary available through the shared context of all four lenses.
- Preserve one authoritative player-standing source per companion while retaining independent companion-to-companion opinions.
- Keep the optional dependency parse-safe when absent.
- Normalize external state into compact facts suitable for deciding impulses, not dialogue instructions or implementation metrics.
- Preserve the external system's independent romantic and physical axes, consent state, and fixed boundaries.

**Non-Goals:**
- Copying or rendering the external mod's character-bio prompt blocks.
- Mutating Relationships state or replacing its spark and consent workflows.
- Supporting bare Romantasy as an authoritative bond source.
- Adding settings, SKSE Menu Framework pages, SkyUI MCM pages, cadence changes, new lenses, or lifecycle behavior.
- Caching preferences before render cost is measured in game.

## Decisions

### Register parse-safe decorator fallbacks only when Relationships is absent

At data load, detect `SNRom_Integration.esl`. When it is absent, register inert stand-ins for `get_romance` and `romance_physical_ok`. The romance fallback returns a valid object with `enrolled` false; the physical fallback returns the exact string form expected by the external decorator.

When the plugin is present, AgencyEngine leaves both names untouched so the Relationships Papyrus quest can register the real decorators during save startup. This mirrors the proven SeverActions compatibility pattern and avoids a premature AgencyEngine registration shadowing the real implementation for the session.

Alternatives considered:
- Guarding unknown decorator calls with `is_plugin_loaded`: rejected because Inja resolves names at parse time.
- Reading Relationships StorageUtil keys directly from AgencyEngine: rejected because those are external persistence internals, while decorators are the intended interoperability boundary.
- Making Relationships a hard dependency: rejected because every lens already has a useful SeverActions or neutral fallback.

The supported contract is Relationships 1.1.2 or newer. Runtime presence detection cannot establish the manifest version, so this is a documented compatibility boundary; optional-field defaults provide graceful behavior for compatible additive schema changes.

### Build the summary inside the shared per-follower standing section

For each follower with a valid SkyrimNet UUID, call `get_romance` once. If `enrolled` is true, render the Relationships player-standing summary and do not call or render `sever_player_blurb` for that follower. If enrollment is false or the fallback object is returned, retain the existing SeverActions player branch unchanged. The SeverActions companion-opinions branch remains unconditional and independent.

Putting the summary in the base makes it available to Aspiration, Relationship, Activity, and Curiosity without four copies that can drift. The base will state the cross-lens invariants: external state is read-only, fixed limits are hard boundaries, and static standing is not proof of a recent transition. Lens files need changes only where their existing subject rules require a more specific interpretation.

Alternative considered: duplicate a Relationships section in each lens prompt. Rejected because all lenses consume the same facts and the repository deliberately centralizes shared context and hard constraints in the base.

### Normalize bond state into six qualitative stages on two independent ladders

Map the external depth value to six concise qualitative descriptions for a platonic bond and six parallel descriptions for a romantic bond. The mapping preserves the source meanings—new acquaintance, growing fondness, friendship, deep trust, costly loyalty, and permanent bond—without emitting numbers or base Romantasy level names. Romantic depths additionally branch on player stance and romantic availability where the external contract does.

Render known decision facts only:
- bond kind and qualitative depth;
- accepted, declined, or unanswered player stance when relevant;
- romantic availability;
- current physical availability, distinguished from permanent physical unavailability through `physMinTier`;
- authored `why` and fixed `limit` text;
- established orientation.

Omit points, raw tier and level names, `toNext`, ardor, exclusivity, address, and all absent optional fields. Empty values are unknown, not neutral defaults. The fixed limit is rendered after licences so its precedence is unambiguous. Physical availability is stated independently from romantic commitment and never described as requiring romance.

Alternative considered: embed the external character-bio prose verbatim. Rejected because it is long, contains dialogue-delivery instructions inappropriate to an ask, duplicates copyrighted prompt content, and would compete with the same prose already active during ordinary dialogue.

### Read preferences in one complete pass

Define the complete 58-entry Romantasy opinion vocabulary as paired faction IDs and AgencyEngine-authored concise labels. Iterate it once per enrolled follower, store each faction rank once, and emit a factual `likes` or `dislikes` entry only for ranks 1 or 0. Negative ranks produce no output. If no opinion exists, omit the preference subsection rather than asserting neutrality.

A single mixed pass performs 58 native rank lookups rather than the external prompt's two grouped passes and 116 lookups. Grouping all likes before dislikes is not worth doubling render work; each emitted line carries its polarity.

Alternatives considered:
- Require an upstream aggregate decorator: rejected because Relationships 1.1.2 does not provide one and the change should work against the selected supported version.
- Add a C++ cache: rejected until profiling establishes a problem; invalidation would be required for manual edits and future mutable preferences.

### Keep transition ownership outside AgencyEngine

The prompt identifies the state as standing evidence, not an action API. No lens may claim to set spark, answer consent, promote a tier, or complete a Relationships transition. Declined stance cannot be reopened, and unanswered stance cannot be presented as mutuality or claim. Physical availability may license an Activity proposal independently of romance, but never compels one; physical unavailability and fixed limits prohibit incompatible proposals.

No parser, persistence, resolver, cue, or pending-impulse changes are required. The model still produces the existing impulse JSON and AgencyEngine handles it through the existing lifecycle.

## Risks / Trade-offs

- [Decorator registration order differs on some load paths] → Register fallbacks only when `SNRom_Integration.esl` is absent, following the existing SeverActions pattern; verify new game, load game, and absent-mod paths in game.
- [An unsupported Relationships schema returns malformed or renamed fields] → Support and document 1.1.2+, use `default` for every optional field, and make enrollment false the safe fallback direction.
- [Preference scans increase prompt-render work with large parties] → Use one 58-lookup pass, inspect SkyrimNet prompt latency in game, and defer caching until measurements justify it.
- [Rich static state tempts lenses to manufacture timely subjects] → Keep existing lens subject and evidence constraints explicit; static standing provides disposition, licence, and depth rather than proof of a recent change.
- [Two systems can discuss the same preference reaction] → Do not add a cross-system suppression mechanism; let the existing recent-history and anti-repeat instructions judge the full exchange, as selected for this change.
- [External prose or labels are inadvertently redistributed] → Author AgencyEngine's concise mappings and labels; consume only exposed decorators and faction state, and credit the optional integration in documentation.

## Migration Plan

1. Ship the DLL fallback registration and prompt changes together so every referenced decorator name is parseable.
2. Existing installations without SkyrimNet Relationships continue through the existing SeverActions or neutral branches without configuration migration.
3. Installations with Relationships begin using its player standing automatically on the next supported render for each enrolled companion; no saved AgencyEngine state changes.
4. Rollback restores the prior DLL and prompt files together. No AgencyEngine or Relationships persistence requires migration or repair.