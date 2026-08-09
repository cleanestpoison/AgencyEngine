# The config file records deviations, and the lens roster is not one of them

`AgencyEngine.json` used to be a full snapshot: `Save` wrote every key, and `Load` read each one over
its default. That is an *upgrade* bug in two layers.

The first is general. Once anyone pressed Save — for any reason, on any tab — the file named every
setting, so from then on no default this project ever retunes could reach that install. A release that
changes `forcedImpulseChance` because forced turns read badly changes it for new installs and nobody
else, and nothing in the file distinguishes a value someone chose from one that was merely current
when they last saved.

The second is specific to lenses, and louder: the file held the whole lens roster as an array, which
`Load` applied wholesale so that a lens someone deleted stayed deleted. The consequence is that a lens
shipped in a later version does not appear at all on an install that already has a settings file. The
Activity lens shipped that way, and the release notes had to ask people to add a row by hand.

So: **the file records what the user changed, and the lens roster moves into the build.**

## Considered Options

- **Ship a filled-in `AgencyEngine.json` in the archive.** Rejected, and it is the intuitive fix. It
  makes the pinning universal and immediate — every install freezes every default on the day it is
  installed, rather than on the day someone first presses Save. It also cannot win the VFS: the DLL
  writes to `Data/SKSE/Plugins/`, which lands in MO2's overwrite, and overwrite shadows the mod folder,
  so the shipped copy is dead weight the moment anyone saves. What ships instead is
  `AgencyEngine.json.example`, which is never read; `package.ps1` refuses to build an archive
  containing a real one.
- **Merge the shipped lens list into the configured one on load.** Rejected: a lens someone deleted
  reappears on the next load, which is the one behaviour nobody expects from a list they edited. This
  is what made wholesale replacement the deliberate choice originally, and it is only escapable by
  giving up the idea that the list is theirs to edit.
- **Version the config and migrate on each bump.** Rejected as the general mechanism — it needs a
  migration written for every default anyone ever retunes, and the one nobody writes is silent. Kept
  for the one place a migration is genuinely unavoidable: reading the old lens array.
- **Track per-field "user has touched this" flags.** Rejected: the same information is already
  available by comparing against the shipped value, and a flag can disagree with the value it guards.

## Consequences

- `Save` writes only fields differing from a default-constructed `Settings`. An untouched install
  saves `{}`. The log line names the count, because an almost-empty file otherwise reads as a failed
  write.
- **A default is now a live decision, not a starting point.** Changing one in `Settings.h` changes it
  for every install that has not moved that particular control. That is the point, and it is also the
  new risk: the blast radius of editing a default is everyone.
- The roster is `kBuiltinLenses` in `Settings.h`. A lens's `name`, `prompt` and `proposal` are content
  — they describe a prompt file that ships in the same archive — and the config cannot contradict them.
  Only `weight` and `ledgerSlots` are preferences, stored as overrides under a stable `Lens::id`. On
  the Lenses tab a shipped row shows those three greyed instead of offering them as text fields; the
  only thing typing into them could ever achieve was pointing a lens at a prompt that doesn't resolve,
  which costs the whole impulse and reads as a lens that is always quiet.
- **`id`, not `name`, keys an override**, so a lens can be renamed by a release without resetting
  anyone's weight. The ledger's rings still key on `name` — they are per-character saved state, and
  rekeying them would strand every slot recorded before the change.
- Lenses a user writes themselves have no shipped row to override and are stored whole, under
  `customLenses`. They occupy the table slots after the builtins.
- Migration from the old array matches rows on `prompt`, carries `weight` and `ledgerSlots` across, and
  sets `weight = 0` for a builtin the file didn't list — deleting a row was how the old page said
  "never ask this", and weight 0 is how the new one says it. A user's rename of a shipped lens is
  dropped, which orphans that row's ledger ring into the shared one; logged, and the shared ring
  already exists to hold exactly that.
- Tested offline in `tests/SettingsTests.cpp`, for the reason the ledger is: these failures are not
  reproducible by hand once the old file has been overwritten.
- Found while writing those tests, and worth recording because it silently produces the exact bug this
  ADR removes: **MSVC constant-initializes a `static const` aggregate to zeros and skips every default
  member initializer.** `static const Settings shipped{}` therefore compared against an empty
  `followerEventTypeFilter` and a roster of blank lenses, and wrote back every field whose default is
  not zero. The baseline is a plain local.
