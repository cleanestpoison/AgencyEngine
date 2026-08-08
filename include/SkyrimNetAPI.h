#pragma once

// Typed, defensive wrapper over SkyrimNet's soft-loaded public C++ API.
//
// PublicAPI.h defines its function pointers at file scope, so it may only be
// included in exactly one translation unit — src/SkyrimNetAPI.cpp. Everything
// else in AgencyEngine talks to SkyrimNet through this namespace. Every
// wrapper degrades to a harmless default when SkyrimNet is missing or older
// than the API we compiled against.

namespace AgencyEngine::SkyrimNetAPI
{
    // Loads SkyrimNet.dll and resolves the exports we use. Call once on
    // kDataLoaded. Idempotent.
    bool Initialize();

    bool IsAvailable();
    int  GetVersion();

    // JSON array of recent world events. formID 0 = all events; 0x14 = the
    // player's. `filter` is a comma-separated event-type list ("" = all).
    std::string GetRecentEvents(std::uint32_t formID, int maxCount, const std::string& filter);

    // SkyrimNet's internal actor UUID, or 0 if the actor is unknown to it.
    std::uint64_t FormIDToUUID(std::uint32_t formID);

    // True once SkyrimNet's memory/event DB is loaded for the current save.
    bool IsMemorySystemReady();

    // Subscribes to SkyrimNet's dialogue events and keeps a timestamp of the
    // most recent one. Call once, after Initialize().
    //
    // This exists because the Papyrus signals cannot see half a conversation.
    // GetTimeSinceLastAudioEnded tracks NPC speech *audio*; the player's turn
    // makes no audio at all, and neither does the several seconds of LLM
    // generation before a reply reaches TTS. In that window nothing is
    // recording, the speech queue is empty and the audio clock keeps climbing —
    // so all three read "quiet" in the middle of an exchange.
    //
    // A dialogue event is registered the instant a line is submitted, by either
    // side, which is exactly where the audio clock is blind.
    bool StartDialogueClock();

    // Milliseconds since the last dialogue event, or -1 when the clock is not
    // running or nothing has been seen yet. -1 must not be read as "busy" —
    // a fresh save legitimately has no dialogue in it.
    std::int64_t MsSinceLastDialogue();

    // Registers an Inja decorator SkyrimNet can call from any prompt as
    // `<name>(some.UUID)`. This is the only channel that reaches ONE NPC's
    // prompt and nobody else's — see PendingImpulse.h for why every event-based
    // route leaks to bystanders.
    //
    // WARNING: `callback` runs on SkyrimNet's thread, in the middle of rendering
    // a prompt. It gets the resolved actor; read its FormID and nothing else. No
    // other RE:: calls, no main-thread work, no blocking. Return "" for anything
    // it cannot answer — every template that calls a decorator has to guard on
    // empty anyway, so "" is the contract rather than a failure.
    //
    // Also note the constraint from SkyrimNet's own docs: evaluating a mod
    // decorator on an NPC who is neither speaker nor target raises "decorator
    // function not found", so the calling template must be guarded on
    // render_mode. Registration itself is once, after Initialize().
    bool RegisterDecorator(const std::string& name, const std::string& description,
                           std::function<std::string(RE::Actor*)> callback);

    // SkyrimNet's identifier for the current save, or "" when none is loaded (or
    // when SkyrimNet is older than v7 and does not export it). Used to key our
    // sidecar files per playthrough.
    std::string GetSaveUniqueID();

    // Renders Data/SKSE/Plugins/SkyrimNet/prompts/<promptName>.prompt against
    // `contextJson` and sends it to the configured LLM.
    //
    // WARNING: `callback` is invoked on a SkyrimNet worker thread, not the
    // game's main thread. Do not touch game objects from it — marshal to the
    // main thread with SKSE::GetTaskInterface()->AddTask first.
    //
    // Returns false if the call could not be queued at all.
    bool SendCustomPromptToLLM(const std::string& promptName,
                               const std::string& variant,
                               const std::string& contextJson,
                               std::function<void(std::string response, bool success)> callback);
}
