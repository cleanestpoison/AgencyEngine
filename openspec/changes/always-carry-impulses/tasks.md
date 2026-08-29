## 1. Enforce the carry invariant

- [x] 1.1 Remove `pendingBioInjection` from the settings model, JSON load/save, runtime diagnostics, and shipped configuration example; add or update settings coverage to verify an old key is ignored and serialization omits it, then run the focused settings tests.
- [x] 1.2 Collapse `CarryImpulse` to the carry-first path so pending-entry creation always precedes optional thought generation and cueing, remove the thought-only fallback, and verify the plugin builds successfully.

## 2. Remove the preference from both interfaces

- [x] 2.1 Remove the bio-injection control, conditional status text, and dependent disabled states from SKSE Menu Framework; verify the menu builds and the remaining settings retain their intended bindings.
- [x] 2.2 Remove the bio-injection key, default, option row, callbacks, and help dispatch from SkyUI MCM, compact every later boolean index consistently, compile the updated Papyrus source to `AgencyEngine_MCM.pex`, and verify each surviving boolean key maps to its intended option.

## 3. Align documentation and release artifacts

- [x] 3.1 Update user-facing documentation and code comments to describe carrying as invariant and private-thought generation as an optional second call, then verify no supported documentation or UI text describes thought-only delivery.
- [x] 3.2 Build and package the mod, inspect the package for the updated DLL, MCM script, example configuration, and prompt files, and run OpenSpec validation for `always-carry-impulses`.

## 4. Exercise delivery behavior

- [ ] 4.1 In game with optional private thoughts off, exercise SKSE Menu Framework and SkyUI MCM and verify both omit the removed setting while exposing the same remaining controls.
- [ ] 4.2 In game, generate an impulse with private thoughts off and verify it is carried and can be cued without an explicit thought call; then enable private thoughts and verify the additional thought occurs only after the carry.
