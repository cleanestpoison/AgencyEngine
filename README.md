# AgencyEngine

An SKSE plugin for [SkyrimNet](https://www.nexusmods.com/skyrimspecialedition/mods/153017) that lets followers
**start** things. Companions normally only ever react — someone speaks to them, something hits them, they answer.
They never stop in the middle of an ordinary afternoon to say *I want to go home*, or *we've been putting this off*,
or *there's something I haven't told you*. This plugin is the part of them that does.

On its own **in-game** clock, each of its lenses reads what has actually been happening to the player and their
companions, asks the LLM whether any of them has something they'd raise unprompted right now, and — when the answer
is yes — hands that companion the subject to carry and a speaking turn to open it with.

The distinction it's built around: **reaction is when the topic comes from the world; action is when it comes from
inside the NPC.** Aspirations supply the agenda, the scene supplies only the timing, and being slightly out of step
with the moment is the feature.

Inspired by [skyrimnet-plugin-narrative-engine](https://github.com/pdusen/skyrimnet-plugin-narrative-engine), which
works at the level of the whole playthrough rather than the party.

## What it does today

Each **lens** asks on its own in-game clock (Aspiration every 2 game hours, Relationship every 6, Activity every 4,
Curiosity every 6), whenever a follower is present and you're not in combat:

1. Pull the recent SkyrimNet event tail for the player and for each follower.
2. Render that lens's prompt against the state and send it to the configured LLM. Four ship: *Aspiration* ("is
   anyone's agenda being ignored?"), *Relationship* ("is anything unsaid between these people?"), *Activity*
   ("is there something they want to do with these people?") and *Curiosity* ("does a companion have a genuine
   unanswered question about the player?").
3. Read back a decision — `{"speaker", "target", "narration"}`, or `{"speaker": null}` for nobody.
4. On a decision to speak, resolve the named speaker and target against the party and **carry** the stage direction:
   it is held in the DLL and rendered into that companion's own character bio, verbatim, privately, with no LLM
   call to deliver it. One per companion per lens, newest first, so two lenses landing together coexist instead of
   one overwriting the other.
5. Set a **cue** for her: a vague direct narration that grants one exact entry a 30-second floor. If that
   companion speaks after the grant, the entry becomes **raised/unmet** with no resolver call. The owner line is not
   resolution evidence; only a later response or action can settle it. Raised/unmet dialogue uses speaker/target
   participation rather than literal topic words, so paraphrases reach the semantic resolver. Untouched carries
   remain in the background and raised/unmet subjects remain available to answer, but are never introduced as new
   again.
6. Optionally (*Also generate a private thought*, off by default) spend a second call on a private reaction. This is
   not needed to carry or deliver the impulse.

Resolution uses persisted event checkpoints rather than silence. AgencyEngine accepts low-latency SkyrimNet callbacks
and also polls a bounded recent-event tail every 15 active real seconds, recovering callbacks SkyrimNet did not
deliver without counting an event twice. The first poll after load establishes a historical baseline. Every accepted
event receives a per-save sequence, and every open untouched or raised/unmet entry receives a semantic check after
30 newer events; unrelated activity advances that checkpoint instead of postponing it. Automatic paid batches are
globally rate-limited to one every four real minutes, and every independently due entry across followers shares the
request. Manual checks bypass both gates, while a proposal retains one pre-expiry fallback. Per-entry watermarks
prevent success, failure, malformed output, or reload from paying twice for the same evidence.

**Most asks produce silence, by design.** A companion who demands something every two hours is a nag; one who does
it twice in a night is a person. The Lenses tab counts carried and quiet asks separately, per lens, so you can see
the ratio.

An ask that *does* land costs its lens a **cooldown** on top of its interval — 8 game hours for Aspiration, 24 for
Relationship and Curiosity, 48 for Activity — measured from the moment the impulse is recorded rather than from her
saying it. That is what makes not nagging structural: a lens can't come back to a subject inside its cooldown,
because it isn't asked.

Curiosity always targets the player. The other lenses may target another companion instead: an old grievance or
unsaid affection between companions is the one thing SkyrimNet's per-NPC loops structurally can't produce, because
nothing else sees the party as a party.

Combat has a separate, opt-in integration for SkyrimNet trigger authors. AgencyEngine registers the silent,
ephemeral `agencyengine_combat` schema and emits `started`, `ongoing`, and `ended` lifecycle events; it never calls
direct narration for them and disables short-lived scene-context copies. `ongoing` defaults to every 15 active real
seconds. Menus, loading screens, background suspension, and brief `IsInCombat()` drops do not advance that cadence.
The Combat tab's shared exit grace defines both the `ended` boundary and optional continuous-mode release.

The structured payload is `{ "phase", "sequence", "elapsed_seconds" }`. A SkyrimNet trigger can match the event
type and use a schema condition such as `phase equals ongoing`, then choose its own audience and response.

Everything is visible and tunable in-game through two parallel interfaces:

- **SKSE Menu Framework**, under **Agency Engine**, provides Status, Settings, Carried and History pages. Its
  Settings page includes the live lens countdowns and carried/quiet counts alongside the controls.
- **SkyUI MCM**, under **Agency Engine**, provides the same live status, configuration, carried impulses, ledger,
  history, diagnostic context, and actions in a controller-friendly layout. Long entries open in a detail dialog,
  and large carried/ledger lists are paged. It is the supported interface for Skyrim VR.

Both interfaces share one live `Settings` object and the same Director, carried-impulse, ledger, and history state.
Changes apply immediately. MCM writes each configuration change to
`Data/SKSE/Plugins/AgencyEngine.json` automatically; SKSE Menu Framework users persist a group of edits with
**Save settings**.

The MCM requires `AgencyEngine.esp` to be enabled. It keeps the `.esp` extension for ordinary load-order placement
but carries the TES4 ESL flag, so it consumes a light slot rather than a full plugin slot. Its only record is the
MCM quest at local FormID `0x800`, inside the legacy light-plugin range. Skyrim SE/AE loads that natively; Skyrim VR
requires [Skyrim VR ESL](https://github.com/Nightfallstorm/SkyrimVRESL). The plugin has no SkyUI master, and neither
missing SkyUI nor a disabled MCM host prevents the DLL or SKSE Menu Framework UI from running.

**No config file ships with the mod, on purpose.** The file holds only the settings you have actually changed;
everything absent from it is whatever this version of the mod ships, and so follows the mod when a later version
retunes it. A shipped file — or one that wrote back every key whether you'd touched it or not — would freeze every
default at the moment you installed, and every update after that would be fighting it. `AgencyEngine.json.example`
is in the archive as documentation of what the keys are; it is never read, and copying it over the real file is not
a supported way to configure anything (it has comments in it, and JSON has no comments).

## Requirements

- SKSE64 or SKSE VR, plus the matching Address Library/CommonLib runtime support
- **SkyrimNet** (built against beta23-rc2's `CppAPI/PublicAPI.h`; API v9)
- Optional standing integrations:
  - **SeverActions** — player rapport and companion-to-companion opinions
  - **[SkyrimNet Relationships](https://github.com/deadohiosky48/SkyrimNet-Relationships) 1.1.2+** — authoritative
    enrolled player bonds, boundaries and Romantasy preferences; it retains its own Romantasy requirement
- Optional configuration interfaces (install either or both):
  - **SKSE Menu Framework** — optional; provides the full status and diagnostic interface where supported
  - **SkyUI / SkyUI VR** — optional; provides the Mod Configuration Menu, including the VR configuration path
  - On Skyrim VR, **Skyrim VR ESL** is required for the ESL-flagged MCM host

Without either UI dependency, AgencyEngine still runs from its shipped defaults and `AgencyEngine.json`.

## How it hangs together

| File | Role |
|------|------|
| `plugin.cpp` | SKSE entry point, message listener, startup order |
| `src/Director.cpp` | the impulse loop — timing, gating, context assembly, dispatch |
| `src/CombatEpisode.cpp` | testable logical-fight clock shared by combat events and continuous mode |
| `src/SkyrimNetAPI.cpp` | the only TU that includes SkyrimNet's `PublicAPI.h` |
| `src/PapyrusBridge.cpp` | writes events back via `SkyrimNetApi` Papyrus natives |
| `src/MCM.cpp` | validated Papyrus-native settings seam plus stable status/carried/ledger/history snapshots and actions |
| `src/UI.cpp` | SKSE Menu Framework presentation |
| `src/Settings.cpp` | shared JSON-backed config |
| `src/State.cpp` | the one mutex everything shared lives behind |
| `Source/Scripts/AgencyEngine_MCM.psc` | SkyUI MCM presentation and option handling |
| `tools/build_mcm_plugin.py` | reproducibly builds the minimal quest plugin that hosts the MCM |
| `statics/` | mirrors the mod folder and contains compiled scripts plus generated plugin assets |
| `tests/` | offline settings, combat-episode, and ledger contract tests, off unless `AGENCYENGINE_BUILD_TESTS=ON` |

### Threading

Three threads touch shared state, and the split is deliberate:

- **Main thread** — every game read (calendar, followers, combat) happens inside an SKSE task posted by the
  Director, and so does every Papyrus write. The VM is not thread-safe and neither is the actor list.
- **Director thread** — SkyrimNet's DB queries hit SQLite; running them on the main thread would stutter the frame.
- **SkyrimNet worker thread** — the LLM callback lands here. It touches nothing in the game; it copies the response
  and posts the delivery hop back to the main thread.

All of it meets in `State.h`'s single mutex, accessed through `WithState()`.

### Why the Papyrus bridge

SkyrimNet's C++ API is read-mostly — it can query events, memories and context and call the LLM, but the functions
that *create* an event (`RegisterPersistentEvent`, `DirectNarration`, …) are exposed only as Papyrus natives. So the
plugin dispatches a static call into SkyrimNet's own native implementation via the VM. The `ByUUID` overloads are
used on purpose: every argument is then a Papyrus `String`, so nothing has to be packed as a form pointer across the
VM boundary.

### Why nothing an impulse carries is an event

**SkyrimNet stamps an audience onto every event at creation, from proximity and line of sight, and nothing in either
API narrows it.** Measured twice — once as this mod's own `agencyengine_event` type, once as SkyrimNet's own
`npc_thoughts` — and both leaked to a bystanding follower's prompt. The audience follows the *creation path*, not the
event type: anything written through the Papyrus natives is public to the scene. Registering the schema with
`shortLivedEnabled = false` keeps it out of scene context but not out of a bystander's event history.

So a carried impulse is not an event at all. It is held in the DLL and reaches exactly one prompt through a
**decorator** SkyrimNet calls while rendering that character's bio — verbatim, no LLM call, and guarded on
`render_mode` so it renders into her own prompt and nobody else's. The only other per-actor channel is asking
SkyrimNet to generate the content itself (`GenerateNPCThought`), which costs a call and paraphrases; the mod uses it
for her private *reaction* to carrying something, never for the agenda. `PapyrusBridge.h` and `PendingImpulse.h`
record the full finding.

The persistent `agencyengine_event` schema is still registered on every load for prose events the whole scene
should know. The separate `agencyengine_combat` schema is ephemeral, cannot interrupt, and has
`shortLivedEnabled = false`: its lifecycle records are silent trigger inputs rather than narration or scene context.

## Building

Needs Visual Studio 2022 (or Build Tools) with the C++ workload, and vcpkg.

```powershell
pwsh -File build.ps1 build            # configure if needed, then build
pwsh -File build.ps1 build -Preset local-debug
pwsh -File build.ps1 clean
```

Machine-specific paths live in `CMakeUserPresets.json`:

- `VCPKG_ROOT` — vcpkg checkout
- `SKYRIMNET_DIR` — the installed SkyrimNet mod folder (the one containing `CppAPI/PublicAPI.h`)
- `SKYRIM_MODS_FOLDER` — deploy staging root; deliberately **not** the live MO2 mods folder, so a build never
  mutates the active modlist. The build writes `_deploy/AgencyEngine/`; copy that across when you want to test.

The first configure builds CommonLibSSE-NG from source and takes a while.

### Tests

```powershell
pwsh -File build.ps1 test -Preset local-tests   # configure if needed, build, then ctest
```

Deliberately not part of the default build: `AGENCYENGINE_BUILD_TESTS` is `OFF`, so a plain clone builds the plugin
and nothing else. Two targets, on the two things here worth testing offline — both pure state with no game, no
SkyrimNet and no LLM behind them, and both able to regress with no visible symptom:

- the **ledger**, which can quietly break an already-shipped lens (a companion simply stops raising things, days
  later, because another lens evicted her settled subjects);
- the **config**, whose failures are *upgrade* failures — a cadence set two versions ago silently reverting, a
  retuned default never reaching an install that already exists, an old-format file losing what it meant. None of
  those appear in a log, and none are reproducible by hand once the old file has been overwritten.

The tests call both with the arguments the plugin passes; `tests/shim/Logging.h` stands in for the spdlog/SKSE
header, which doesn't exist outside the game process.

Everything else is verified in game, deliberately: there is no offline renderer for Inja or its decorators, so every
prompt change is judged from the per-lens carried/quiet counters, the History page's context payload, the ledger view
and the log lines that name every eviction, suppression and declining path.

### Papyrus

`Source/Scripts/*.psc` are **not** built by CMake — the compiler path is machine-specific and it needs the base-game
and SkyUI script headers, so wiring it in would break a plain C++ build on a fresh clone. Compile by hand and commit
the resulting `.pex` files under `statics/Scripts/`, from where the statics deploy ships them:

```powershell
Get-ChildItem "Source/Scripts/*.psc" | ForEach-Object {
  & "<Skyrim>/Papyrus Compiler/PapyrusCompiler.exe" `
    $_.FullName `
    -i="Source/Scripts;<Skyrim>/Data/Source/Scripts" `
    -o="statics/Scripts" `
    -f="TESV_Papyrus_Flags.flg" -optimize
}
```

The script include folder must contain SkyUI's `SKI_ConfigBase.psc`; SkyUI VR ships the same MCM interface. The
script's own folder must also be on `-i`, or the compiler cannot resolve `AgencyEngine_MCMNative`. Configure warns
when a `.psc` is newer than its `.pex`, because that otherwise fails only at runtime.

The MCM quest plugin is reproducibly generated and checked with:

```powershell
python tools/build_mcm_plugin.py
python tools/build_mcm_plugin.py --check
```

`external/SKSEMenuFramework.h` is vendored from
[QTR-Modding/SKSE-Menu-Framework-3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3); it ships no library and
soft-links `SKSEMenuFramework.dll` at runtime.

## Packaging an installable mod

```powershell
pwsh -File package.ps1                 # builds, then writes out/AgencyEngine-v0.1.0.zip
pwsh -File package.ps1 -Version 0.2.0
pwsh -File package.ps1 -SkipBuild      # package what's already deployed
```

The script builds first (a stale mod folder would ship a broken archive), refuses to package if the DLL, MCM host,
Papyrus scripts or prompt files are missing from the deploy, and zips the *contents* of `_deploy/AgencyEngine/`.
The archive therefore has `AgencyEngine.esp`, `Scripts/` and `SKSE/` directly at its root, which MO2 and Vortex
install without a FOMOD.

The version defaults to the `project(AgencyEngine VERSION ...)` line in `CMakeLists.txt`, so the archive name can't
drift from what the DLL reports to SKSE.

## Cutting a release

```powershell
pwsh -File release.ps1 0.2.0 -DryRun    # every check, no writes
pwsh -File release.ps1 0.2.0            # bump, build, tag, publish
```

`release.ps1` rewrites the version in `CMakeLists.txt`, builds and packages, commits the bump, tags it, pushes, and
creates the GitHub release with the archive attached. `CMakeLists.txt` is the single source of truth, so the version
SKSE reports, the git tag and the archive filename are the same by construction rather than by discipline.

The build happens locally, not on a runner, because compiling needs SkyrimNet's `CppAPI/PublicAPI.h` — that header
ships with the SkyrimNet mod and isn't vendored here, so CI has no way to obtain it. `gh` is used only to publish.

Before anything is written it refuses to proceed on a dirty tree, a branch other than `main`, a local `main` behind
`origin`, or a tag that already exists. It also checks the two things that fail *after* publishing rather than
during: that no `.psc` is newer than its committed `.pex` (which surfaces in-game as "function not registered", not
as a build error), and that no `.pex` header carries the compiling machine's username or hostname — the Bethesda
compiler writes both into every `.pex` and has no flag to suppress it, so a rebuild silently reintroduces them.

A failed build restores `CMakeLists.txt`, and a failed tag push deletes the local tag, so an aborted run leaves
nothing half-done to clean up by hand.

## What gets installed

```
AgencyEngine.esp                                                     (ESL-flagged; hosts the optional SkyUI MCM quest)
SKSE/Plugins/AgencyEngine.dll
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_base.prompt         (the shared spine)
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_aspiration.prompt
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_relationship.prompt
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_activity.prompt
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_curiosity.prompt
SKSE/Plugins/SkyrimNet/config/plugins/AgencyEngine/manifest.yaml
SKSE/Plugins/AgencyEngine.json.example                                 (documentation; never read)
Scripts/AgencyEngine_Bridge.pex
Scripts/AgencyEngine_MCM.pex
Scripts/AgencyEngine_MCMNative.pex
```

There is no `AgencyEngine.json` in that list, and that is the point — see *Settings* above.

Every lens shares the one `agencyengine_impulse` LLM variant — they are the same job at the same cost, so one
variant means one place in SkyrimNet's UI to point impulse generation at a cheaper model.

The two SkyrimNet files sit exactly where SkyrimNet's own auto-discovery looks — the same layout the narrative
engine uses, and the reason `statics/` mirrors the mod folder verbatim rather than being flattened at deploy time:

- **`prompts/<name>.prompt`** — SkyrimNet resolves `SendCustomPromptToLLM("agencyengine_impulse_aspiration", ...)` to
  `Data/SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_aspiration.prompt`. No registration step, purely
  path-based, so the files must land at those paths and must not be overwritten by another mod.
- **`config/plugins/AgencyEngine/manifest.yaml`** — declares the `agencyengine_impulse` LLM *variant*, which is what
  lets you point impulse generation at a cheaper model than your dialogue model from inside SkyrimNet's own UI.
  Without it, impulses silently fall back to the default Dialogue LLM.

Load order relative to SkyrimNet doesn't matter — the DLL resolves SkyrimNet at `kDataLoaded`.

## Lenses

A **lens** is one focused question the loop can ask. Rather than a single prompt that offers the model eight kinds of
thing at once and gets the easiest one back every time, each lens asks for one kind and is told to return silence
when that kind isn't there.

| Lens | Asks | Produces | Cadence (game hours) | Needs |
|------|------|----------|----------------------|-------|
| Aspiration | Is anyone's agenda being ignored? An aspiration walked past, an errand stuck, an order of operations she disagrees with. | Topics | every 2, then quiet for 8 | — |
| Relationship | Is anything unsaid between these people? A settled view one companion has been carrying about another. | Topics | every 6, then quiet for 24 | SeverActions, optional |
| Activity | Is there something they want to *do* with these people? A drink, a round of sparring, a game, restlessness for a fight, an hour of somebody's company. | Proposals | every 4, then quiet for 48 | — |
| Curiosity | Does a companion have a genuine unanswered question about the player? One information gap, with understanding the player as the payoff. | Topics | every 6, then quiet for 24 | — |

Aspiration is the workhorse and asks often. Relationship's material accumulates over in-game days, while Curiosity
waits for a genuine unknown and a moment when asking fits; both use the slower 6/24 cadence. Activity's danger is
repeat-proposing — "spar with me" again tonight — so it takes the longest cooldown.

Aspiration deliberately does **not** own mundane appetites — rest, food, a bed, a drink. Those are Activity's, and
leaving them in both would split one register across two names instead of asking two questions.

### Every cadence is in game time, so your timescale sets the bill

The intervals above are **in-game** minutes, and an ask is an LLM call whether or not anyone had anything to say.
What turns one into the other is Skyrim's `TimeScale` — how many in-game minutes pass in a real one. At vanilla 20
the shipped roster asks about twenty-two times an hour played; at the 6 to 10 heavier modlists run, the identical
settings ask a third as often. Nothing about the config changes; the bill does.

So the settings page reads the live timescale and prints the real-time length beside every in-game duration it
shows — each lens's interval and cooldown, its countdown to the next ask, the carry TTL, and every clock on the
Carried page. Resolution has no game-time cadence: each entry advances every 30 accepted events, with a four-real-minute
minimum between automatic paid batches.
The log quotes both units on every ask. A timescale of `0` — some mods freeze the clock that way — means no lens will ever
come due, and both the Status page and the log say so outright rather than showing a countdown that never moves.

The Lenses tab can also set the timescale, because it is the number the cadence sliders are denominated in. It is
the *game's* setting, though — shared with every other mod and saved in the save file — so AgencyEngine writes it
when you press the button and never stores it, never puts it back at load, and never fights whatever else set it.
Below **7** the warning on that control is about Skyrim rather than about this mod: NPC packages stop completing,
travel and shop restocking stall, and timer-driven mods drift.

**The roster is not configuration.** Which lenses exist, what each is called, which prompt file it asks through and
whether it produces proposals all come from the mod, because they describe prompt files that ship in the archive
beside the DLL — there is nothing there a settings page could usefully let you author, and a prompt name that
doesn't resolve costs the whole impulse and reads as a lens that is simply always quiet. What is yours is the
**switch**, the **interval**, the **cooldown** and the **slot count**, and those are all the config file stores.

So a lens added in a later version turns up on its own, at its shipped cadence, in a settings file that already
exists — and a lens whose prompt or cadence is retuned in a later version is retuned for you too, unless you moved
that control yourself. Unticking a lens is how you switch one off; an install upgraded from the version before
per-lens cadence keeps every lens it had switched off (a stored weight of 0 means exactly that), and any lens it had
deleted outright comes back switched off rather than running.

Lenses you write yourself are a separate list and are stored whole — see *Writing your own lens* below.

They share a spine. `agencyengine_impulse_base.prompt` owns the JSON contract, the hard constraints, the state dump
and the forced-turn machinery; each lens `{% extends %}`es it and may override ten prose blocks (`lens_task`,
`lens_why_now`, `lens_speaker_target`, `lens_when_to_speak`, `lens_examples`, `lens_subject_source`,
`lens_player_context`, `lens_focus`, `lens_evidence_check`, `lens_user_question`). Fixing a shared constraint means
editing one file, and the lenses can't drift apart on the output format.

No block sits inside an `{% if %}` or a `{% for %}`, and no lens reads a variable the base sets. Blocks nested in
control flow and cross-template variable scope are the two parts of Inja inheritance that weren't worth betting an
impulse on — a render error costs the whole call and fails as a blank prompt rather than as anything you'd notice.
All branching is base-owned; lenses supply prose.

**Cadence is in the DLL, not the template, and there is no selection left to make.** Each lens holds a game-time
deadline; a pass of the Director checks the clocks and asks whatever is due, which is usually nothing. Two
consecutive asks can't repeat a question for free — a lens is structurally unable to re-ask inside its own interval,
because it isn't asked. Several clocks coming due together is ordinary and simply produces several asks; a template
couldn't do any of this, because a template can't remember anything.

**Unticking a lens switches it off**, which is the escape hatch for a lens whose prompt needs a mod you don't have.

Standing integrations are optional for every lens. The shared base calls SeverActions'
`sever_player_blurb` / `sever_companion_opinions` and SkyrimNet Relationships'
`get_romance` / `romance_physical_ok`. Inja resolves unknown functions when it *parses* a file, before a template
guard can help, so the DLL registers inert stand-ins only when the plugin owning each decorator is absent. All four
lenses therefore remain usable with neither integration installed.

Player standing has one authority per companion. If SkyrimNet Relationships 1.1.2+ reports that companion enrolled,
its qualitative bond, recorded stance, availability, authored reason and limit, established orientation, and all
authored Romantasy likes/dislikes replace the SeverActions player blurb. Missing Relationships fields stay unknown;
they are not filled from a second source. If the companion is not enrolled, the existing SeverActions player blurb
remains the fallback. SeverActions companion-to-companion opinions remain independent and render in either case.

This integration is automatic and read-only. There is no AgencyEngine setting or UI row for it, and AgencyEngine
does not initiate or record Relationships spark, consent, bond or progression transitions. It consumes the external
mod's exposed state without shipping that mod's files or copying its character-bio prompts.

Standing remains steady-state: it supplies *what* exists, never *why now*. Stamped thoughts and events still supply
timing, and the per-lens carried/quiet counters on the Lenses tab remain the readout. A relationship lens that is
silent 22 times out of 23 is an argument for tracked threads, not for treating a static bond as a new event.

There is no general prompt behind the lenses. Switching every lens off doesn't fall back to anything — it stops
the loop, and the Status page says so rather than quietly asking a broader question.

### Topics and proposals

Each lens declares whether it produces **topics** or **proposals**, and two things branch on that declaration. It is
declared, never inferred from the lens's name — for a shipped lens it comes from the mod along with the prompt file,
and for one you wrote yourself it is a checkbox on the row. Inferring it from the name would mean a rename silently
changing how an impulse resolves, which fails as wrong behaviour rather than as an error.

A **topic** is met when somebody answers it: agreeing, refusing, arguing it out. A **proposal** asks the party to
*do* something, and agreement doesn't settle it — "yes, let's spar" followed by no sparring is a deferral, and
deferral is not an answer. So the resolution check keeps a proposal live until the events show the thing happened
or somebody plainly refused, which is the only thing that makes a proposal land at all.

The **ledger rings are per lens** for the same reason. Proposals come from a closed vocabulary — a companion has
perhaps ten activity subjects in her life, against unlimited unique topics — so a shared six-slot ring would hold
her whole repertoire, veto her into silence, and evict genuine aspiration and relationship subjects on the way.
Each lens now evicts only within its own ring, and its size is the `Slots` column (0 = use the global number on the
Impulses tab). Rendering stays combined: the prompt sees one "already raised" list, so no lens repeats another's
subject.

An untouched entry creates provisional personal memory. Its floor-owned raise confirms personal memory across that
companion's lenses and party-heard memory across followers. Untouched retirement withdraws its provisional record;
raised/unmet retirement preserves confirmed records. Personal insertion/eviction remains per lens, while party
memory retains at most 32 exact subjects for seven game days.

There is a **shared ring** behind the per-lens ones, holding anything raised before the rings existed and anything
raised by a lens that has since been renamed or deleted. Its slots suppress for every lens and are only ever
evicted by another shared-ring slot — so a lens with three slots can't drop six settled subjects on the first ask
after an upgrade, and renaming a row doesn't strand what it had already settled. They leave one at a time, as their
subjects come round again and get rewritten under a live lens.

### Writing your own lens

The blank rows at the bottom of the Lenses tab are yours. Fill in a name, a prompt file (which resolves to
`Data/SKSE/Plugins/SkyrimNet/prompts/<name>.prompt` and must `{% extends %}` `agencyengine_impulse_base.prompt`), an
interval, a cooldown, whether it produces proposals, and its slot count. Seven lenses is the table's limit, of
which four are the mod's own.

A lens you wrote has no shipped row behind it, so unlike the mod's own it is stored whole in the config file, under
`customLenses` — nothing in an update can add to it, change it or take it away.

## Tuning the impulse

The prompts are normal SkyrimNet Inja templates split into `[ system ]` / `[ user ]` sections. Editing one and
rebuilding redeploys it without touching the DLL.

The one knob that changes how often companions speak is **Force someone to speak** on the Impulses tab, and it
applies to every lens. It is a percent chance that the turn is *forced*: the silence option is removed from the
template entirely and someone has to speak up. The rest of the time the model may return silence, and normally will.
`0` disables forcing; `100` makes every turn speak, which shows — a forced impulse on a thin day is the weakest thing
the prompt writes.

The DLL sends the number into the template as `forced_impulse_chance` and the roll stays there, next to the
`tail_live` half of the condition that suppresses it. `agencyengine_impulse_base.prompt` keeps a `20` fallback behind
an `exists()` guard, for the case where the prompt file is newer than the DLL rendering it.

The roll is taken **once** into `{% raw %}{% set roll = random %}{% endraw %}` and reused at every branch. `random`
re-evaluates on each reference, so testing it directly in more than one place lets the roll disagree with itself and
offer the silence option in one section while forbidding it in another — a bug SkyrimNet's own
`dialogue_speaker_selector.prompt` has.

The Lenses tab's "N carried, M quiet" counters are the readout for tuning this: raise the chance if it is all
quiet, lower it if the companions start sounding scheduled.

## Roadmap

Tier 1 — *she brings it up* — is what's implemented. The two tiers above it share all of its infrastructure:

- **Tier 2: it's a demand, and it's tracked.** Per-follower threads that persist across impulses with an age and a
  history, instead of the agenda being re-derived from scratch every time. Raised and ignored twice, a want comes
  back harder. This is what separates "the mod said a thing" from "she's been on about this for a week".
- **Tier 3: she does it anyway.** Refused enough times, she stops following, walks to the door, leaves for a day.
  The tier where the player learns the demands are real — which is what retroactively makes tiers 1 and 2 mean
  something.

Nearer-term, cheaper:

- Filtering combat hit-lines out of the event tail — they crowd out the material that carries interiority.
- A cue that can tell "she has one thing on her mind" from "she has three", if the play pass shows stacked
  impulses coming out as one overloaded turn rather than one thing at a time.

## License

MIT — see [LICENSE](LICENSE). `external/SKSEMenuFramework.h` is vendored from
[QTR-Modding/SKSE-Menu-Framework-3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) and remains under its own
terms.
