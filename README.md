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

Each **lens** asks on its own in-game clock (Aspiration every 2 game hours, Relationship every 6, Activity every 4),
whenever a follower is present and you're not in combat:

1. Pull the recent SkyrimNet event tail for the player and for each follower.
2. Render that lens's prompt against the state and send it to the configured LLM. Three ship: *Aspiration* ("is
   anyone's agenda being ignored?"), *Relationship* ("is anything unsaid between these people?") and *Activity*
   ("is there something they want to do with these people?").
3. Read back a decision — `{"speaker", "target", "narration"}`, or `{"speaker": null}` for nobody.
4. On a decision to speak, resolve the named speaker and target against the party and **carry** the stage direction:
   it is held in the DLL and rendered into that companion's own character bio, verbatim, privately, with no LLM
   call to deliver it. One per companion per lens, newest first, so two lenses landing together coexist instead of
   one overwriting the other.
5. Set a **cue** for her: a vague direct narration — she has something on her mind — that grants her a speaking
   turn and names no subject, because the bio already carries it. One pending cue per companion however many
   impulses she picks up, and it waits for the party to go quiet before it goes out. She writes her own line; the
   plugin only supplies the agenda.
6. Optionally (*Also generate a private thought*, on by default) ask SkyrimNet for an unvoiced thought from the
   speaker about what she has just decided she wants to raise. It is audienced to her alone and lands in her event
   history — which is where this mod's own prompt reads thoughts back from, so it is the one thing in the loop
   that carries forward. It generates asynchronously and so colours what she says from the *next* call onward.

**Most asks produce silence, by design.** A companion who demands something every two hours is a nag; one who does
it twice in a night is a person. The Lenses tab counts carried and quiet asks separately, per lens, so you can see
the ratio.

An ask that *does* land costs its lens a **cooldown** on top of its interval — 8 game hours for Aspiration, 24 for
Relationship, 48 for Activity — measured from the moment the impulse is recorded rather than from her saying it.
That is what makes not nagging structural: a lens can't come back to a subject inside its cooldown, because it
isn't asked.

The target need not be the player. A companion raising something *with another companion* — an old grievance, an
unsaid affection — is the one thing SkyrimNet's per-NPC loops structurally can't produce, because nothing else sees
the party as a party.

Everything is visible and tunable in-game through the **SKSE Menu Framework** control panel, under a
**Agency Engine** section with three pages:

- **Status** — SkyrimNet connection, current followers, each lens's countdown to its next ask, and a
  *Generate an impulse now* button.
- **Settings** — gating, cues, how much context to feed the model, and the Lenses tab: a switch, an interval, a
  cooldown and a slot count per lens, with its countdown, its carried/quiet count and an *Ask now* button beside
  them. *Ask now* asks that one lens immediately, whatever its clock says — the button for tuning a single prompt.
  It spends the clock like any other ask.
- **History** — the last 25 impulses with the lens each was asked under, plus the exact context JSON of the most recent dispatch.

Settings persist to `Data/SKSE/Plugins/AgencyEngine.json` (press *Save settings*; they load on game start).

**No config file ships with the mod, on purpose.** The file holds only the settings you have actually changed;
everything absent from it is whatever this version of the mod ships, and so follows the mod when a later version
retunes it. A shipped file — or one that wrote back every key whether you'd touched it or not — would freeze every
default at the moment you installed, and every update after that would be fighting it. `AgencyEngine.json.example`
is in the archive as documentation of what the keys are; it is never read, and copying it over the real file is not
a supported way to configure anything (it has comments in it, and JSON has no comments).

## Requirements

- SKSE64, Address Library
- **SkyrimNet** (built against beta23-rc2's `CppAPI/PublicAPI.h`; API v9)
- **SKSE Menu Framework** — optional but strongly recommended; without it the plugin runs headless on defaults.

## How it hangs together

| File | Role |
|------|------|
| `plugin.cpp` | SKSE entry point, message listener, startup order |
| `src/Director.cpp` | the impulse loop — timing, gating, context assembly, dispatch |
| `src/SkyrimNetAPI.cpp` | the only TU that includes SkyrimNet's `PublicAPI.h` |
| `src/PapyrusBridge.cpp` | writes events back via `SkyrimNetApi` Papyrus natives |
| `src/UI.cpp` | SKSE Menu Framework pages |
| `src/Settings.cpp` | JSON-backed config |
| `src/State.cpp` | the one mutex everything shared lives behind |
| `statics/` | mirrors the mod folder; the `.prompt` file deploys from here |
| `tests/` | the offline ledger tests, off unless `AGENCYENGINE_BUILD_TESTS=ON` |

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

The `agencyengine_event` schema is still registered on every load — schemas live in SkyrimNet's DLL rather than in
the save — and the bridge can still write one, for anything the whole scene *should* know. Nothing on the impulse
path does.

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
script headers, so wiring it in would break a plain C++ build on a fresh clone. Compile by hand and commit the
resulting `.pex` under `statics/Scripts/`, from where the statics deploy ships it like any other file:

```powershell
& "<Skyrim>/Papyrus Compiler/PapyrusCompiler.exe" `
  "Source/Scripts/AgencyEngine_Bridge.psc" `
  -i="Source/Scripts;<Skyrim>/Data/Source/Scripts" `
  -o="statics/Scripts" `
  -f="TESV_Papyrus_Flags.flg" -optimize
```

The script's own folder must be on `-i` as well as the base-game sources; without it the compiler fails with
"unable to locate script" for the very file it was handed. Configure warns when a `.psc` is newer than its `.pex`,
because the failure mode otherwise is a runtime "function not registered" rather than a build error.

`external/SKSEMenuFramework.h` is vendored from
[QTR-Modding/SKSE-Menu-Framework-3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3); it ships no library and
soft-links `SKSEMenuFramework.dll` at runtime.

## Packaging an installable mod

```powershell
pwsh -File package.ps1                 # builds, then writes out/AgencyEngine-v0.1.0.zip
pwsh -File package.ps1 -Version 0.2.0
pwsh -File package.ps1 -SkipBuild      # package what's already deployed
```

The script builds first (a stale mod folder would ship a broken archive), refuses to package if the DLL or the
prompt file is missing from the deploy, and zips the *contents* of `_deploy/AgencyEngine/` so the archive root is
`SKSE/` — the shape MO2 and Vortex install directly, with no FOMOD needed for a single-folder mod.

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
SKSE/Plugins/AgencyEngine.dll
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_base.prompt         (the shared spine)
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_aspiration.prompt
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_relationship.prompt
SKSE/Plugins/SkyrimNet/prompts/agencyengine_impulse_activity.prompt
SKSE/Plugins/SkyrimNet/config/plugins/AgencyEngine/manifest.yaml
SKSE/Plugins/AgencyEngine.json.example                                 (documentation; never read)
Scripts/AgencyEngine_Bridge.pex
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

Aspiration is the workhorse and asks often. Relationship's material accumulates over in-game days, so a long
interval buys fewer quiet asks against the same standing data rather than more answers. Activity's danger is
repeat-proposing — "spar with me" again tonight — so it takes much the longest cooldown of the three.

Aspiration deliberately does **not** own mundane appetites — rest, food, a bed, a drink. Those are Activity's, and
leaving them in both would split one register across two names instead of asking two questions.

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
and the forced-turn machinery; each lens `{% extends %}`es it and overrides six prose blocks (`lens_task`,
`lens_when_to_speak`, `lens_examples`, `lens_focus`, `lens_subject_source`, `lens_user_question`). Fixing a
constraint means editing one file, and the lenses can't drift apart on the output format.

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

The relationship lens is the one case of that, and it doesn't need the escape hatch: it calls SeverActions'
`sever_player_blurb` and `sever_companion_opinions` decorators, and Inja resolves unknown functions when it
*parses* a file — so a flag inside the template couldn't save an install without SeverActions. The DLL registers
inert stand-ins under both names when SeverActions isn't in the load order, and the template already treats an
empty blurb as "nothing settled yet". **SeverActions is therefore optional**: install it and the lens has standing
data to work from, leave it out and the lens still runs on thoughts and events alone.

Expect the relationship lens to be quieter than the aspiration one, and expect that to be honest. Standing is
steady-state: the blurbs carry no timestamp and no delta, so they supply the *subject* and never the *why now*. The
per-lens carried/quiet counters on the Lenses tab are the readout — a lens that is silent 22 times out of 23 is an
argument for tracked threads (tier 2), not for loosening its prompt.

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

A slot is written the moment a subject is carried, not when she says it: a cue grants her a turn without naming
which subject she'll raise, so carry is the only event this mod can actually observe. It stays *provisional* until
the resolution check confirms it (the subject was met, so it stays suppressed) or something else clears the impulse
(nobody answered, so it goes back into circulation).

There is a **shared ring** behind the per-lens ones, holding anything raised before the rings existed and anything
raised by a lens that has since been renamed or deleted. Its slots suppress for every lens and are only ever
evicted by another shared-ring slot — so a lens with three slots can't drop six settled subjects on the first ask
after an upgrade, and renaming a row doesn't strand what it had already settled. They leave one at a time, as their
subjects come round again and get rewritten under a live lens.

### Writing your own lens

The blank rows at the bottom of the Lenses tab are yours. Fill in a name, a prompt file (which resolves to
`Data/SKSE/Plugins/SkyrimNet/prompts/<name>.prompt` and must `{% extends %}` `agencyengine_impulse_base.prompt`), an
interval, a cooldown, whether it produces proposals, and its slot count. Six lenses is the table's limit, of which
three are the mod's own.

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
