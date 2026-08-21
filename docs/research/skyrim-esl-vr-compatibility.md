# ESL-flagged ESP compatibility for AgencyEngine

Accessed 2026-08-21.

## Question

Can the single-record `AgencyEngine.esp` MCM host be ESL-flagged while remaining compatible with Skyrim SE/AE and Skyrim VR? Its only new record is a start-game-enabled `QUST` whose object ID is `0x800`.

## Recommendation

**Ship `AgencyEngine.esp` with the TES4 ESL flag and keep the `.esp` extension.** The plugin is technically eligible:
its sole new record is a `QUST` at local FormID `0x800`, inside the conservative light-record range, and neither a
quest nor attached VMAD disqualifies it. Skyrim SE/AE loads it natively. AgencyEngine's supported Skyrim VR path
requires **Skyrim VR ESL Support**, which supplies the missing light-plugin loader and FormID mapping; stock VR
without that runtime patch can still run the DLL headless, but cannot use the MCM host.

SKSEVR, CommonLibVR, and SkyUI VR do not themselves add light-plugin loading. SKSEVR is the mechanism through which
the separate loader patch is installed; CommonLibVR supplies C++ types/relocation infrastructure; and SkyUI VR
supplies the UI/MCM surface. SkyUI VR's own installation material lists SKSE VR as its prerequisite and makes no
ESL-loader claim, while the ESL-support project implements and installs the missing loader hooks separately
([SkyUI VR README](https://github.com/Odie/skyui-vr/blob/master/README.md),
[SkyrimVRESL README and developer API](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/README.md),
[SkyrimVRESL flag/mapping hooks](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/src/eslhooks.h)).

## What “ESL” means

Three separate properties are easy to conflate:

1. **Filename extension.** `.esp` and `.esl` are names/suffixes. Bethesda calls `.ESL` files “light master files,” but that support article describes the Creations load-order UI, not the binary eligibility rules ([Bethesda Support](https://help.bethesda.net/app/answers/detail/a_id/63978/~/why-cant-i-change%2Fsave-my-creations-load-order%3F)).
2. **TES4 header flag.** The light/small-file bit is `0x200`. xEdit explicitly supports “the ESL flag in the file header” separately from the `.esl` extension, which implicitly sets light behavior ([xEdit changelog, ESL support](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#esl-support)). SkyrimVRESL likewise reads `0x200` into `kSmallFile`, while a filename ending in `.esl` forces both master and small-file flags ([`ESLFlagHook::SetFormFlag`](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/src/eslhooks.h#L34-L63)). Therefore an **`.esp` can carry the TES4 ESL flag**; changing the suffix to `.esl` is neither necessary nor equivalent to merely flagging an ESP.
3. **Compact local FormID space.** A light plugin receives a light slot and its runtime IDs are mapped into `FE xxx yyy`: xEdit documents 4,096 light slots in `FE000`–`FEFFF`, and SkyrimVRESL constructs a runtime ID as `0xFE000000 | (smallFileCompileIndex << 12) | (localID & 0xFFF)` ([xEdit](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#esl-support), [`AdjustFormIDFileIndex`](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/src/eslhooks.h#L82-L102)). The light index is assigned at load time; it must not be hard-coded into SKSE/CommonLib plugin code.

The ESL flag does not itself reorder a plugin: xEdit says light modules can load before full modules and that the FormID prefix is not a reliable load-order indicator ([xEdit](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#esl-support)).

## FormID eligibility

For the broadly compatible, pre-expanded Skyrim light format, use local object IDs **`0x800` through `0xFFF`**. The upper bound is directly enforced by xEdit: it refuses to save an ESL-flagged module containing a new record above `0xFFF` ([xEdit](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#esl-support)). The conservative lower bound is corroborated by the VR implementation itself: it treats `0x000`–`0x7FF` as reserved before assigning a plugin/master index ([`IsFormIDReserved`](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/src/eslhooks.h#L104-L107)). xEdit does expose an expert extended range of `0x001`–`0xFFF`, and later xEdit work specifically mentions backported/extended-low-range handling, but that is not a safe baseline across old SE runtimes and VR without a matching backport ([xEdit extended-range note](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#whats-new-in-xedit-416h)).

Consequently, a sole new `QUST` at local object ID **`0x800` is eligible without compacting or renumbering**. It is exactly the first ID in the conservative range and below xEdit's `0xFFF` ceiling. “Start Game Enabled” is a quest flag/behavior, not an additional record and not part of xEdit's light-module eligibility check. The relevant structural check is whether *every new record* fits the compact range; this plugin has only the one stated new record.

## Skyrim SE/AE versus Skyrim VR

### Skyrim SE/AE

SE/AE's loader recognizes the TES4 ESL flag, assigns a light index, and maps records into the `FE` space; xEdit states that its mapping matches the game engine ([xEdit](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#esl-support)). Thus `AgencyEngine.esp` could remain named `.esp`, set the TES4 ESL bit, and load as a light plugin on supported SE/AE runtimes.

### Skyrim VR

There is no Bethesda primary documentation specifying VR light-plugin behavior. The strongest available evidence is executable-patch source, so this conclusion is explicit reverse-engineering evidence rather than a Bethesda guarantee:

- xEdit enables ESL handling in TES5VREdit **only when it detects the installed “Skyrim VR ESL Support” SKSE plugin** ([xEdit, “ESL supported in Skyrim VR”](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#esl-supported-in-skyrim-vr)).
- The support plugin says its purpose is to “Add[] ESL support to SkyrimVR” and requires the VR Address Library ([SkyrimVRESL README](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/README.md)).
- Its implementation replaces VR's TES4 flag handling, recognizes header bit `0x200` and `.esl`, strips a light record to 12 local bits, and rebuilds `FE + 12-bit light index + 12-bit local ID`; it also hooks Papyrus `GetFormFromFile` to use that mapping ([`eslhooks.h`](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/src/eslhooks.h#L18-L63), [`AdjustFormIDFileIndex` and `PapyrusGetFormFromFileHook`](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/src/eslhooks.h#L82-L151)).

Those patches would be redundant if stock VR already performed the same work. Accordingly, **stock Skyrim VR should be treated as not recognizing/mapping ESL-flagged ESPs**. Skyrim VR can load them only when the separate SkyrimVRESL runtime (and its prerequisites) is installed. This is also why merely compiling AgencyEngine's DLL against SKSEVR/CommonLibVR, or using SkyUI VR for MCM, cannot make the data plugin light: the missing behavior is in the game's plugin loader and FormID paths, which SkyrimVRESL hooks.

## Quest and Papyrus/VMAD caveats

A `QUST` with attached Papyrus metadata (`VMAD`) is not categorically disqualified. xEdit's compatibility rule is framed in terms of new-record object IDs, not record signature or the presence of VMAD ([xEdit](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#esl-support)). SkyrimVRESL also patches Papyrus `GetFormFromFile` so a local ID is resolved through the owning light file ([source](https://github.com/Nightfallstorm/SkyrimVRESL/blob/dev/src/eslhooks.h#L109-L151)).

Compaction is nevertheless hazardous when it actually renumbers records. xEdit's `Compact FormIDs for ESL` changes new IDs above `0xFFF` to free IDs at or below `0xFFF` and explicitly warns that it **does not rename external files whose filenames contain FormIDs** ([xEdit, “Compact FormIDs for ESL”](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/whatsnew.md#compact-formids-for-esl)). Creation Kit quest fragments are generated Papyrus scripts, so fragment-bearing quests and compiled scripts/properties deserve a post-compaction audit; generated fragment filenames, VMAD script names/properties, and any external FormID-derived assets must remain consistent. This is a caveat, not evidence that all VMAD quests are unsafe.

For AgencyEngine specifically, no compaction is required because the quest is already `0x800`. Setting a header flag would not change that local object ID. If the generated ESP later gains another new record, or if the quest acquires fragments/aliases/properties that reference new forms, re-run xEdit's ESL compatibility/error checks and inspect VMAD before reconsidering the flag.
AgencyEngine's native sources also contain no lookup, stored constant, or filename reference for this host FormID (`src/` and `include/` search), so the DLL does not depend on a full-plugin runtime prefix; that removes one common technical obstacle to flagging, but it does not remove VR's loader dependency.

## Decision

| Runtime | ESL-flagged `AgencyEngine.esp` | Condition |
|---|---|---|
| Skyrim SE/AE | Supported | All new local IDs remain in the compatible compact range; current sole `QUST:00000800` qualifies. |
| Skyrim VR, stock loader | Not supported for the MCM host | VR does not natively supply the SE ESL flag/FormID mapping evidenced above. |
| Skyrim VR + SkyrimVRESL | Supported | SkyrimVRESL and its prerequisites provide the light-plugin loader. |

AgencyEngine deliberately chooses the ESL-flagged ESP. This saves a full plugin slot on SE/AE and on the supported
VR setup without renumbering any record. The file remains named `AgencyEngine.esp`; only TES4 header bit `0x200` is
set. The generator must continue rejecting any new local FormID outside `0x800`–`0xFFF`.

