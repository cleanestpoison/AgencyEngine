## 1. Simplify Impulse Prompts

- [x] 1.1 Rewrite the shared base prompt's existing state-support constraint to require evidence for subject, register, and attributed motive, change its legacy forced-chance fallback to 0, and verify the inherited JSON, reaction, silence, anti-repeat, and Inja block contracts remain intact by focused source review.
- [x] 1.2 Cut the Activity lens's subject catalogs and content-bearing examples, express its voluntary-shared-experience boundary once across the existing override blocks, and verify the file retains only prose overrides supported by the base template.
- [x] 1.3 Cut the Relationship lens's attitude catalogs and content-bearing examples, make state choose subject and register, and verify an observed action without speaker interpretation is no longer presented as sufficient motive evidence.
- [x] 1.4 Remove detailed cross-lens social examples from Aspiration and any remaining built-in override that advertises a loaded subject elsewhere, and verify each rejection names only the semantic boundary between lenses.

## 2. Retune Shipped Defaults

- [x] 2.1 Change the shipped forced-impulse chance to 0 and total recent-event budget to 70 in the settings source of truth and configuration example, then extend config tests to verify fresh defaults, absent-key adoption, explicit-override preservation, and deviations-only saving.
- [x] 2.2 Update SKSE Menu Framework controls and guidance to identify 0 percent and 70 events as the shipped defaults without changing valid ranges, and verify the plugin build succeeds.
- [x] 2.3 Update the parallel SkyUI MCM controls, summaries, and detail text with the same values and behavioral meaning, and verify its displayed setting metadata matches SKSE Menu Framework.
- [x] 2.4 Update README tuning guidance and prompt/default documentation to describe evidence-led subject selection, default silence, and the 70-event window, and verify no current user-facing documentation claims the former 20-percent or 40-event defaults.

## 3. Automated Verification

- [x] 3.1 Run `pwsh -File build.ps1 test -Preset local-tests` and verify the complete config and ledger test suite passes with the new defaults and preserved explicit overrides.
- [x] 3.2 Run `pwsh -File build.ps1 build` and verify the deployable plugin and prompt assets build successfully without template-file or resource-copy errors.

## 4. In-Game Behavioral Verification

- [ ] 4.1 With a default configuration, open both SKSE Menu Framework and SkyUI MCM in game and verify each shows forced chance 0 and event budget 70 with equivalent ranges and explanations.
- [ ] 4.2 Set non-default forcing and event-budget values through one interface, verify the other interface reflects the same live values and that they survive save/reload, then remove the deviations and verify both return to 0 and 70.
- [ ] 4.3 In a party with at least two companions, exercise at least five Activity asks and five Relationship asks across quiet and evidence-bearing states; inspect the rendered history contexts and outcomes to verify silence remains available, no stock competitive or confrontational repertoire is supplied, and unsupported motives are not assigned to ordinary actions.
- [ ] 4.4 Exercise supported warm, neutral, and sharp companion states and verify the generated register follows the supplied characterization rather than enforcing positivity or confrontation; inspect carried entries and dialogue in both user-facing history views before release.
