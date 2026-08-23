# SkyrimNet Concepts

Orientation doc for agents working on SkyrimNet integrations. Explains *what the moving
parts are and how they feed each other* — not how to author any one file. For the
step-by-step authoring workflows see `modding/WORKFLOW_*.md`; for template syntax see
`modding/prompt-file-syntax.md`.

---

## 1. What SkyrimNet is

A single native SKSE plugin (`SkyrimNet.dll`) that turns every NPC into an LLM-driven
conversationalist. No external process, no Python, no WSL — the DLL reads game state
straight from memory, calls an OpenAI-compatible endpoint (OpenRouter by default), and
speaks the reply through a TTS engine. A web dashboard runs at `localhost:8080`, and an
MCP server at port `8889` exposes 44+ tools to external AI assistants.

Everything an LLM sees is assembled by **Inja templates** (`.prompt` files) that pull live
game data through **decorators**. That means most behaviour is changeable without touching
C++: edit a prompt, drop in a YAML action, add a YAML trigger.

---

## 2. The central data model

Four stores feed almost every prompt. Understanding how they relate is most of
understanding SkyrimNet.

```
   game / mod events ──► EVENT HISTORY ──┬──► dialogue prompt ──► LLM ──► spoken line
                                         │                                    │
                                         └──► (periodic) MEMORY GENERATION     │
                                                        │                      │
   CHARACTER BIO ◄── (periodic dynamic bio update) ◄─────┘                      │
        │                                                                      │
        └────────────► dialogue prompt ◄── relevant MEMORIES (vector search) ◄──┘
                              ▲
                              └── SCENE CONTEXT (who's here, where, what time, what's equipped)
```

### 2.1 Event history

A rolling, timestamped log of *everything that happened*. An "event" is deliberately broad:

| Family | Example types |
|---|---|
| Dialogue | `dialogue`, `dialogue_npc`, `dialogue_player`, `dialogue_player_stt`, `dialogue_player_text`, `dialogue_background` |
| Inner life | `npc_thoughts`, `player_thoughts` |
| Narration | `direct_narration` (narration nearby NPCs treat as fact), persistent events |
| Combat / world | `combat`, `hit`, `spell`, `active_effect`, death, location change |
| GameMaster | `gamemaster_dialogue` |
| Mod-defined | anything registered via `RegisterEventSchema` (e.g. `baka_defeat`, Devourment swallow events) |

Key properties:

- Each event carries `originatingActor`, `targetActor`, `location`, `gameTime`, `type`,
  and a typed `data` payload validated against its schema.
- Events render through **format modes** — `recent_events`, `raw`, `compact`, `verbose` —
  chosen by the consuming prompt. The same event looks different in scene context, in
  event history, and in a memory-generation prompt.
- Event history is the **conversation transcript** fed to the dialogue model. In
  `components/event_history.prompt` the target NPC's own events become `assistant` turns
  and everyone else's become `user` turns, so the model literally sees the scene as a chat
  it is participating in.
- Elapsed time is narrated, not raw: gaps become `*The next day...*`, `*A few hours
  later...*`, `[Yesterday]` prefixes, and location changes are announced inline.
- Events can be **ephemeral** (auto-expiring with a TTL) or persistent, and can be flagged
  `interrupt` to cut off in-progress speech.

Custom event types are registered from Papyrus with
`SkyrimNetApi.RegisterEventSchema(eventType, displayName, description, fieldsJson,
formatTemplatesJson, isEphemeral, ttlMs, shortLivedEnabled, interrupt)`, then emitted with
`RegisterEvent` / `RegisterEventByUUID` / `RegisterShortLivedEvent`.

### 2.2 Character bios

The character sheet. Assembled from numbered submodule files in
`prompts/submodules/character_bio/` — the numeric prefix is the render order:

`0100_summary`, `0130_world_knowledge`, `0200_background`, `0300_personality`,
`0320_aspirations`, `0400_appearance`, `0410_equipment`, `0500_skills`,
`0600_relationships`, `0610_party_quests`, `0700_occupation`, `7100_memories`,
`9990_speech_style`.

- Rendered at several **detail levels** via `render_character_profile(variant, uuid)`:
  `full`, `short_inline`, `dialogue_target`, `interject_inline`. Cheap prompts use the
  short variants; the speaking NPC gets `full`.
- ~3,000 hand-written bios ship for vanilla and popular mod NPCs. Anyone without one gets
  a bio generated from their game data (`helpers/generate_profile.prompt`).
- **Static vs dynamic bio.** Static is the authored baseline. The *dynamic* bio is
  periodically rewritten by an LLM from recent events + important memories
  (`dynamic_bio_update.prompt`, configurable under Advanced Configuration → DynamicBio),
  so a character drifts as the playthrough goes on. Papyrus can force one with
  `UpdateActorDynamicBio(actor)`.

### 2.3 Memory

Per-NPC, first-person, tagged, and semantically searchable. Written by an LLM from event
history + that NPC's bio (`memory/generate_memory.prompt`), producing:

```json
{ "content": "...", "location": "...", "emotion": "joyful|…|traumatized",
  "importance_score": 0.0-1.0, "tags": ["whiterun","trading",…],
  "type": "EXPERIENCE|RELATIONSHIP|KNOWLEDGE|LOCATION|SKILL|TRAUMA|JOY" }
```

- **Retrieval is two-stage.** A local GPU embedding model does vector recall over the
  NPC's memory store, then (optionally) `memory/memory_ranker.prompt` re-ranks candidates
  against the current situation — mood, location, recent events, player relationship. A
  helper model can also write the search query (`helpers/generate_search_query.prompt`).
  The winners are injected by `components/memory_access.prompt`.
- **Importance and decay.** Being attacked outranks small talk; memories fade at a rate
  proportional to importance.
- **Diaries** (`diary_entry.prompt`) summarise an NPC's day and become searchable memories
  themselves. `GenerateDiaryEntry(actor)` forces one.
- Memory is *private to the NPC*. Shared facts belong in world knowledge.

### 2.4 World knowledge

Facts larger than one character — "A dragon attacked Helgen", "The Stormcloaks took
Whiterun". Scoped with short template conditions like `is_in_location("Whiterun")` or
`get_quest_stage("MQ104") >= 13`, so a fact enters the world exactly when a quest milestone
fires and reaches only the characters it should. Each entry is either **always injected**
(when its condition passes) or pulled in **semantically** when relevant. Bundle entries
into **Knowledge Packs** (`.sknpack`) to ship lore with a quest mod. **NPC Groups**
("Whiterun Guards") are user-defined actor sets referenced from conditions, triggers, and
chat.

---

## 3. Who talks, and when

### Perception and scene context

An NPC only reacts to what they could plausibly see or hear — people upstairs don't answer
conversations downstairs. Whisper mode shrinks the radius further. `get_scene_context(npc,
target, "full")` supplies the "who is here, where, what time, what's happening" block.

### Speaking-turn selection

After each line, a lightweight model decides **which single NPC speaks next, if anyone**
(`target_selectors/dialogue_speaker_selector.prompt`), weighing relationships,
personality, and whether anyone has a compelling reason to react. If nobody does, the
chain ends. This is what makes conversations stop naturally instead of ping-ponging
forever.

### GameMaster

A periodic director that *initiates* interaction rather than responding to it. Two stages:

- `gamemaster_scene_planner.prompt` — writes a loose 4–6 beat scene plan (tone, central
  tension, beats) for the NPCs present. Its hard constraint: **the player is
  uncontrollable**, so scenes must work entirely among NPCs.
- `gamemaster_action_selector.prompt` — picks the concrete next move.

GameMaster output enters history as `gamemaster_dialogue` events. **Continuous scene mode**
is the related feature where NPCs keep a conversation going with each other while you
watch. Toggle both from Papyrus (`TriggerToggleGameMaster`, `TriggerToggleContinuousMode`).

### Thoughts and telepathy

NPCs can hold unvoiced `npc_thoughts` that influence later dialogue without being heard
(`GenerateNPCThought(actor, promptHint)`, or `/npcthink` in chat). An optional telepathy
perk lets the player send private dialogue to one NPC; a lesser power lets them eavesdrop
on nearby thoughts. When a thought is surfaced to another actor telepathically, the event
history templates explicitly mark it `(thought, telepathically observed)` so the model
doesn't mistake it for speech.

---

## 4. Extension surfaces

### Actions — what an NPC can *do*

YAML in `SKSE/Plugins/SkyrimNet/config/actions/`. An action maps an NPC intent to a
Papyrus function on a quest script:

```yaml
name: DevourTarget
description: Devours or swallows a target actor whole. Use when {{ npc.name }} wants to…
questEditorId: DevourmentManager      # quest holding the script
scriptName: devourmentmanager
executionFunctionName: VoreTarget
parameterMapping:                      # speaker | dynamic (LLM-filled) | static
  - { type: speaker, name: pred }
  - { type: dynamic, name: targetActor, description: The target actor to be devoured }
  - { type: static,  name: Endo, value: false }
eligibilityRules:                      # decorator-based gating, AND/OR groups
  - conditions:
      - { decoratorName: is_in_combat, arguments: [currentActor], comparisonOperator: "==", expectedValue: false }
    required: true
```

- `description` is the *only* thing the model reads when choosing — write it as an
  instruction to the character, not as developer documentation.
- **Eligibility is evaluated in engine, before the LLM sees the list**, and is cached and
  pre-warmed while your voice is still transcribing. Ineligible actions are never offered.
- Actions can be nested into **categories** (`cat_*.yaml`) so the model picks a high-level
  intent first and drills down with a cheaper prompt.
- Alternatives to YAML: `SkyrimNetApi.RegisterAction` at runtime from Papyrus, or native
  C++ from another SKSE plugin.

### Triggers — react to anything

YAML in `config/triggers/`. "When event X happens, do Y":

```yaml
name: baka_defeat
eventCriteria:
  eventType: baka_defeat          # any registered event type, incl. mod_event, spell_cast, active_effect
response:
  type: direct_narration          # or player_thought / player_dialogue / npc_thought /
                                  # persistent_event / diary / dynamic_bio / virtual NPC toggle / voice effect
  content: |
    {{ target }} has been overpowered. {{ originator }} stands over them. …
audience: nearby_npcs
enabled: true
probability: 1
cooldownSeconds: 10
priority: 10
```

Conditions support nested field access, numeric/string comparison, regex, template
expressions, cooldowns, probability, priority, and dialogue-interrupt flags.

### Prompts and decorators

`.prompt` files under `SKSE/Plugins/SkyrimNet/prompts/`, Inja syntax (`{{ }}` expressions,
`{% %}` control flow, `{# #}` comments). Layout:

| Path | Role |
|---|---|
| `*.prompt` (root) | Entry points: `dialogue_response`, `npc_thoughts`, `diary_entry`, `native_action_selector`, `gamemaster_*` |
| `components/` | Reusable blocks: `event_history*`, `memory_access`, `character_bio_*`, `context/` |
| `submodules/<group>/NNNN_*.prompt` | Ordered fragments composed by `render_subcomponent(group, variant)` |
| `memory/`, `helpers/`, `omnisight/`, `target_selectors/`, `transformers/` | Per-subsystem prompts |
| `original_prompts/` | Pristine shipped copies — diff against these to see local edits |

Over a hundred built-in **decorators** expose live state (`decnpc(uuid)`, `get_name`,
`get_location`, `get_recent_events`, `get_relevant_memories`, `is_in_combat`,
`is_in_faction`, `get_quest_stage`, …). Mods register their own with
`SkyrimNetApi.RegisterDecorator(id, scriptName, functionName)`. Templates hot-reload — no
game restart.

`[ system ] / [ user ] / [ assistant ]` markers inside a prompt split the rendered text
into chat roles.

### OmniSight (vision)

Screenshots actors, items, locations, furniture, and scenes; a vision model describes them
and the descriptions flow back into prompts, so an NPC can comment on your strange amulet
or the fact that the room is on fire. Descriptions can be locked to protect curated wording.

### Papyrus API (`SkyrimNetApi.psc`)

The main entry points a mod integration uses:

- **Events:** `RegisterEventSchema`, `RegisterEvent(ByUUID)`, `RegisterShortLivedEvent`,
  `FormatEvent`, `ValidateEventData`
- **Speech:** `RegisterDialogue(ToListener)`, `DirectNarration`, `RegisterPersistentEvent`,
  `TransformDialogue`, `GenerateNPCThought`, `PurgeDialogue`
- **Actions:** `RegisterAction`, `RegisterSubCategory`, `RegisterTag`, `ExecuteAction`,
  `SetActionCooldown`
- **Content:** `UpdateActorDynamicBio`, `GenerateDiaryEntry`, `RenderTemplate`,
  `SendCustomPromptToLLM`
- **Identity:** `GetEntityUUID(actor)` — most APIs have a `…ByUUID` twin, which is the only
  way to address **virtual NPCs** (narrators, inner voices, disembodied speakers that have
  no in-game body but can still talk, remember, and react)
- **Misc:** AI package control, config get/patch, hotkey triggers, virtual NPC lifecycle

Lifecycle ModEvents are available too (`SkyrimNet_SpeechStarted`,
`SkyrimNet_MemoryCreated`, `SkyrimNet_DiaryCreated`, …).

---

## 5. Model roles

Different jobs run on different models, all configured under Advanced Configuration →
OpenRouter. Use a cheap fast model for the high-frequency meta jobs and a strong one for
dialogue.

| Role | Job |
|---|---|
| **Text Generation** (default) | The actual in-character dialogue |
| **Game Master** | Scene planning and initiating NPC-driven interaction |
| **Memory Generation** | Writes and scores memories from recent events |
| **Profile Generation** | Creates bios for NPCs that don't ship with one |
| **Dynamic Profile Updates** | Periodically rewrites bio sections from recent events |
| **Action Evaluation** | Picks which action an NPC takes |
| **Mood Evaluation** | Emotional state → voice modulation + facial expression |
| **Speaking Turn Evaluation** | Who speaks next, or nobody |
| **Memory Search** | Writes the semantic query used for memory retrieval |

---

## 6. Working notes for agents

- **MCP is the source of truth over source code.** The MCP server on `:8889` can list
  registered actions/event types, check eligibility for a real actor, render any prompt
  template with full context, and validate trigger/action YAML before saving. Prefer
  querying it over reasoning from files.
- **Config is MO2-virtualised.** Any mod can ship `SKSE/Plugins/SkyrimNet/config/...` and
  it is picked up automatically — which also means several mods may contribute files with
  the same name, and load order decides. In this workspace, `[No Delete] SkyrimNet - My
  Merged Edits` and `[No Delete] SkyrimNet Devourment Scripts Overwrite` are the folders
  that win.
- **Distinguish the three "why did the NPC know that?" paths.** Event history (short-term,
  literal, everyone nearby), memory (long-term, private, semantic), world knowledge
  (shared, condition-scoped). Putting a fact in the wrong one is the most common
  integration bug.
- **Prompts and triggers hot-reload; Papyrus does not.** Editing a `.prompt` or YAML takes
  effect live (or via an MCP reload); changing a Papyrus function called by an action means
  a recompile and redeploy.
- **Descriptions are prompts.** Action `description`, event `formatTemplates`, and trigger
  `content` are all read by an LLM in character context. Write them in second person to the
  character, not as API docs.
