#pragma once

// SkyrimNet's C++ API is read-mostly: it can query events, memories and
// context, and it can call the LLM — but the functions that *create* an event
// are exposed only as Papyrus natives (SkyrimNetApi.psc). So writing an event
// means dispatching a static Papyrus call into SkyrimNet's own native
// implementation.
//
// All functions here MUST be called on the game's main thread (the Papyrus VM
// is not thread-safe); use SKSE::GetTaskInterface()->AddTask.
//
// The ByUUID overloads are deliberate: passing SkyrimNet's uint64 actor UUIDs
// as strings keeps every argument a Papyrus String, so nothing has to be
// packed as a form pointer across the VM boundary. UUID 0 means "unspecified"
// and is sent as an empty string.
//
// GenerateNPCThought is the exception, because SkyrimNet ships no ByUUID
// overload of it. CommonLibSSE packs a raw form pointer for us
// (PackValue -> PackHandle for anything is_form_pointer_v), so this costs an
// Actor* argument rather than any hand-rolled object binding — but it does
// mean the caller must hold a live actor, not just an ID.

namespace AgencyEngine::PapyrusBridge
{
    // Our own SkyrimNet event type — one type for everything this mod records,
    // discriminated by EventRecord::kind rather than split into a type per
    // feature. A new kind costs a string constant and nothing else; a new
    // *type* would cost a schema, a registration call and a format template.
    //
    // The type exists because RegisterPersistentEvent has no audience. Its
    // events get a short-lived scene-context copy (RegisterEventSchema's
    // shortLivedEnabled, default true), and components/context/
    // component_recent_events.prompt renders scene.short_lived_events into
    // every NPC in the scene with no participant filter at all — so an impulse
    // meant for one follower was read by everyone standing nearby. Registering
    // our own schema with shortLivedEnabled = false keeps the event out of
    // scene context entirely, leaving components/event_history.prompt as the
    // only path to a prompt — and that one filters on get_recent_events(n,
    // [npc.UUID]), so only the actors named on the event see it.
    inline constexpr auto kEventType = "agencyengine_event"sv;

    // Values for EventRecord::kind. Add here, not in a new event type.
    inline constexpr auto kKindImpulse = "impulse"sv;

    // One record written to SkyrimNet's event history. Every field is emitted
    // into the payload even when empty, so a format template never renders a
    // key the data object doesn't have.
    struct EventRecord
    {
        // Required by the schema. `kind` is the discriminator, `msg` is the
        // prose NPCs actually read.
        std::string_view kind;
        std::string      msg;
        // Display names, for the diagnostic format modes. The prompt template
        // prefixes the speaker's name itself from the UUIDs below, so these are
        // not what attributes the event.
        std::string      speaker;
        std::string      target;
        // Free-form. `detail` is whatever a future kind needs to say about
        // itself without earning a schema field.
        std::string      lens;
        std::string      detail;
        // Who the event is *about*. These, and only these, decide who reads it.
        std::uint64_t    speakerUuid = 0;
        std::uint64_t    targetUuid = 0;
    };

    // SkyrimNetApi.RegisterEventSchema for kEventType. Schemas live in
    // SkyrimNet's DLL, not in the save, so this must run on every load — the
    // same arrangement other SkyrimNet integrations use. Safe to call
    // repeatedly; re-registration overwrites.
    //
    // Returning true means the call was dispatched, not that SkyrimNet accepted
    // it (natives answer through an asynchronous stack callback). A dispatch
    // that never succeeded makes RecordEvent fall back to the old broadcast
    // path rather than write events against a type that may not exist.
    bool RegisterEventSchemas();

    // SkyrimNetApi.RegisterEventByUUID — writes `record` to the event history
    // under kEventType.
    //
    // NOT PRIVATE, AND CANNOT BE MADE PRIVATE. SkyrimNet stamps an audience
    // onto every event at creation from proximity and line of sight (SKSEHelpers
    // FilterLineOfSight; EventUtils logs "beyond 2* the interaction distance"),
    // and nothing in the Papyrus or C++ API takes an audience argument —
    // originator and target are labels for rendering, not scope.
    //
    // Measured twice. An impulse written as kEventType was read by a bystanding
    // follower's dialogue prompt. Rewriting the same impulse as SkyrimNet's own
    // npc_thoughts type did not help either, even with a schema-valid payload —
    // while in that same prompt, eight thoughts SkyrimNet generated for the
    // speaker herself did not appear at all. The audience follows the creation
    // path, not the event type: anything written through the Papyrus natives is
    // public to the scene. For one actor only, ask SkyrimNet to generate the
    // content itself (GenerateNPCThought) or write a memory (PublicAddMemory,
    // owned by a single FormID) instead.
    //
    // So: use this for things the whole scene should know.
    //
    // Falls back to RegisterPersistentEvent when the schema was never
    // registered: a recorded-but-public impulse is a worse outcome than a
    // private one, but it is a much better outcome than a lost one.
    bool RecordEvent(const EventRecord& record);

    // SkyrimNetApi.RegisterPersistentEventByUUID — persisted to the event
    // history and injected into *every* nearby NPC's context, with dialogue
    // reactions disabled. Kept as RecordEvent's fallback; prefer RecordEvent,
    // which can be addressed.
    bool RegisterPersistentEvent(const std::string& content,
                                 std::uint64_t originatorUuid,
                                 std::uint64_t targetUuid);

    // SkyrimNetApi.DirectNarrationByUUID — same, but NPCs may respond out
    // loud. `originatorUuid` is who the narration is attributed to.
    bool DirectNarration(const std::string& content,
                         std::uint64_t originatorUuid,
                         std::uint64_t targetUuid);

    // SkyrimNetApi.GenerateNPCThought — asks the LLM for an unvoiced thought
    // for `actor` and stores it as an EVENT_NPC_THOUGHTS event audienced to
    // them alone. Private, never spoken, and it surfaces in that NPC's own
    // later prompts — including ours, which reads thoughts back in.
    //
    // Returning true means SkyrimNet accepted the request, not that a thought
    // was produced: generation is asynchronous, and the call skips silently
    // when the NPC is on the cooldown configured in NpcThoughts.yaml.
    bool GenerateNPCThought(RE::Actor* actor, const std::string& promptHint);

    // AgencyEngine_Bridge.SetContinuousMode — our own Papyrus script, shipped as
    // statics/Scripts/AgencyEngine_Bridge.pex.
    //
    // SkyrimNet's continuous scene mode has a toggle and a query but no setter,
    // so switching it *to* a known state is a read-modify-write. That whole
    // sequence lives in Papyrus rather than here, because a native's return
    // value only reaches C++ through an asynchronous stack callback — the query
    // and the toggle would straddle an unbounded gap, and a player pressing the
    // hotkey inside it gets flipped the wrong way.
    //
    // Returning true means the call was dispatched, not that the mode changed.
    // The outcome arrives separately, through the mod event below.
    bool SetContinuousMode(bool desired);

    // Listens for AgencyEngine_ContinuousMode, the event the Papyrus helper
    // sends once it has done the work, and records the outcome in Status:
    // whether continuous mode is on, whether *we* were the ones who turned it
    // on (and therefore owe a turn-off), and whether the toggle was ignored
    // because SkyrimNet's GameMaster agent is disabled.
    //
    // Safe to call more than once; only the first registration takes effect.
    void RegisterContinuousModeSink();

    // AgencyEngine_Bridge.PollQuiet — samples IsRecordingInput,
    // GetSpeechQueueSize and GetTimeSinceLastAudioEnded in one Papyrus stack
    // and reports them through the AgencyEngine_Quiet mod event.
    //
    // All three are Papyrus-only. Without them the best available signal is the
    // timestamp on the last dialogue event, which misses both the generation
    // window (decided but not yet spoken) and the player holding the
    // microphone — the two states it is most important not to interrupt.
    //
    // Returning true means the call was dispatched, not that a reading arrived.
    bool PollQuiet();

    // Listens for AgencyEngine_Quiet and stores the reading in Status. Polling
    // rather than a push, because SkyrimNet publishes no "conversation started"
    // event and two of the three signals are level, not edge.
    //
    // Safe to call more than once; only the first registration takes effect.
    void RegisterQuietSink();
}
