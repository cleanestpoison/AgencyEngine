#include "SkyrimNetAPI.h"

// The ONLY translation unit that may include this — PublicAPI.h defines its
// function pointers at file scope, so a second include would be a duplicate
// definition at link time.
#include <PublicAPI.h>

#include "Logging.h"


namespace AgencyEngine::SkyrimNetAPI
{
    namespace
    {
        bool g_initialized = false;
        bool g_available = false;

        // Written from SkyrimNet's ThreadPool, read from the game's main thread
        // and the Director's. An atomic is the whole synchronisation story: the
        // callback touches nothing else, which is what makes it safe to run
        // there at all.
        std::atomic<std::int64_t> g_lastDialogueMs{ 0 };
        std::vector<std::uint64_t> g_dialogueCallbacks;
        std::mutex g_rawDialogueLock;
        std::deque<RawDialogueEvent> g_rawDialogue;
        constexpr std::size_t kRawDialogueCap = 256;

        std::int64_t SteadyNowMs();


        std::int64_t SteadyNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    }

    bool Initialize()
    {
        if (g_initialized) {
            return g_available;
        }
        g_initialized = true;

        g_available = ::FindFunctions();
        if (!g_available) {
            logger::error("SkyrimNet: SkyrimNet.dll not found (or PublicGetVersion missing) — AgencyEngine will "
                          "idle. Check that SkyrimNet is installed and enabled.");
            return false;
        }

        logger::info("SkyrimNet: connected (API version {})", ::PublicGetVersion ? ::PublicGetVersion() : -1);

        // Each export resolves independently, and an older SkyrimNet leaves the
        // newer ones null. Report exactly which of the ones we depend on are
        // present, so a partial-capability install reads as such in the log
        // rather than as a mystery at first use.
        const std::pair<const char*, bool> required[]{
            { "PublicGetRecentEvents", ::PublicGetRecentEvents != nullptr },
            { "PublicFormIDToUUID", ::PublicFormIDToUUID != nullptr },
            { "PublicIsMemorySystemReady", ::PublicIsMemorySystemReady != nullptr },
            { "PublicSendCustomPromptToLLM", ::PublicSendCustomPromptToLLM != nullptr },
            { "PublicRegisterDecorator", ::PublicRegisterDecorator != nullptr },
        };
        for (const auto& [name, resolved] : required) {
            if (resolved) {
                logger::info("SkyrimNet:   {} resolved", name);
            } else {
                logger::error("SkyrimNet:   {} MISSING — this SkyrimNet is older than AgencyEngine expects", name);
            }
        }

        // Not fatal: without a save ID the pending-impulse sidecar has nothing
        // to key off, so that feature degrades to session-only and everything
        // else is unaffected.
        if (!::PublicGetSaveUniqueID) {
            logger::warn("SkyrimNet:   PublicGetSaveUniqueID missing (needs SkyrimNet v7+) — pending impulses will "
                         "not survive a reload");
        }

        return g_available;
    }

    bool IsAvailable()
    {
        return g_available;
    }

    int GetVersion()
    {
        if (!g_available || !::PublicGetVersion) {
            return -1;
        }
        return ::PublicGetVersion();
    }

    std::string GetRecentEvents(std::uint32_t formID, int maxCount, const std::string& filter)
    {
        if (!g_available || !::PublicGetRecentEvents) {
            return {};
        }
        try {
            auto events = ::PublicGetRecentEvents(formID, maxCount, filter.c_str());
            logger::debug("SkyrimNet: GetRecentEvents(formID={:08X}, max={}, filter='{}') -> {} bytes", formID,
                          maxCount, filter, events.size());
            if (events.empty()) {
                logger::debug("SkyrimNet: no recent events for {:08X} — the impulse will have little to work from",
                              formID);
            }
            return events;
        } catch (const std::exception& e) {
            logger::error("SkyrimNet: GetRecentEvents threw: {}", e.what());
            return {};
        }
    }

    std::uint64_t FormIDToUUID(std::uint32_t formID)
    {
        if (!g_available || !::PublicFormIDToUUID) {
            return 0;
        }
        return ::PublicFormIDToUUID(formID);
    }

    bool IsMemorySystemReady()
    {
        if (!g_available || !::PublicIsMemorySystemReady) {
            return false;
        }
        return ::PublicIsMemorySystemReady();
    }

    bool RegisterDecorator(const std::string& name, const std::string& description,
                           std::function<std::string(RE::Actor*)> callback)
    {
        if (!g_available || !::PublicRegisterDecorator) {
            logger::error("SkyrimNet: PublicRegisterDecorator is missing — this SkyrimNet is older than v5, so the "
                          "'{}' decorator cannot be registered and any prompt calling it will fail to parse",
                          name);
            return false;
        }

        if (::PublicHasDecorator && ::PublicHasDecorator(name.c_str())) {
            // Registering over an existing name fails, and the existing one is
            // almost certainly ours from a previous kDataLoaded. Say so rather
            // than logging a bare failure.
            logger::info("SkyrimNet: decorator '{}' is already registered — leaving it alone", name);
            return true;
        }

        try {
            const bool ok = ::PublicRegisterDecorator(name.c_str(), description.c_str(), std::move(callback));
            if (ok) {
                logger::info("SkyrimNet: registered decorator '{}'", name);
            } else {
                logger::error("SkyrimNet: refused to register decorator '{}' (name conflict, or the callback was "
                              "rejected). Prompts calling it will fail to parse.",
                              name);
            }
            return ok;
        } catch (const std::exception& e) {
            logger::error("SkyrimNet: RegisterDecorator('{}') threw: {}", name, e.what());
            return false;
        }
    }

    std::string GetSaveUniqueID()
    {
        if (!g_available || !::PublicGetSaveUniqueID) {
            return {};
        }
        try {
            return ::PublicGetSaveUniqueID();
        } catch (const std::exception& e) {
            logger::error("SkyrimNet: GetSaveUniqueID threw: {}", e.what());
            return {};
        }
    }

    void EnqueueRawDialogue(RawDialogueEvent event)
    {
        std::scoped_lock lock{ g_rawDialogueLock };
        if (g_rawDialogue.size() == kRawDialogueCap) {
            g_rawDialogue.pop_front();
        }
        g_rawDialogue.push_back(std::move(event));
    }

    std::vector<RawDialogueEvent> DrainRawDialogueEvents()
    {
        std::scoped_lock lock{ g_rawDialogueLock };
        std::vector<RawDialogueEvent> result;
        result.reserve(g_rawDialogue.size());
        while (!g_rawDialogue.empty()) {
            result.push_back(std::move(g_rawDialogue.front()));
            g_rawDialogue.pop_front();
        }
        return result;
    }

    bool StartDialogueClock()
    {
        if (!g_available || !::PublicRegisterEventCallback) {
            logger::warn("SkyrimNet: PublicRegisterEventCallback is missing — cannot watch dialogue, so impulses "
                         "will only avoid interrupting NPC *speech*, not the player's half of a conversation");
            return false;
        }
        if (!g_dialogueCallbacks.empty()) {
            return true;
        }

        // Deliberately NOT "dialogue_background": that is ambient chatter from
        // NPCs standing nearby, which is not a conversation the party is in and
        // must not hold an impulse back indefinitely in a busy town.
        //
        // Deliberately NOT "direct_narration" either — that is how our own
        // impulses are delivered, and letting one reset the clock would mean
        // every impulse made the next one look like an interruption.
        for (const auto* type : { "dialogue", "dialogue_npc", "dialogue_player", "dialogue_player_text",
                                  "dialogue_player_stt", "combat", "world", "active_effect", "death",
                                  "location_change" }) {
            try {
                const auto id = ::PublicRegisterEventCallback(
                    type, [typeName = std::string{ type }](const char* payload) {
                        if (typeName.starts_with("dialogue")) {
                            g_lastDialogueMs.store(SteadyNowMs(), std::memory_order_relaxed);
                        }
                        EnqueueRawDialogue(ParseEventCallbackPayload(payload ? payload : "", typeName, SteadyNowMs()));
                    });
                if (id != 0) {
                    g_dialogueCallbacks.push_back(id);
                } else {
                    // Registering a type this SkyrimNet never emits is not an
                    // error worth shouting about; the others still cover us.
                    logger::debug("SkyrimNet: no dialogue callback registered for '{}'", type);
                }
            } catch (const std::exception& e) {
                logger::error("SkyrimNet: RegisterEventCallback('{}') threw: {}", type, e.what());
            }
        }

        if (g_dialogueCallbacks.empty()) {
            logger::error("SkyrimNet: no dialogue event callbacks registered — conversation detection will miss the "
                          "player's turn entirely");
            return false;
        }

        logger::info("SkyrimNet: watching {} dialogue event type(s) to tell a conversation from a lull",
                     g_dialogueCallbacks.size());
        return true;
    }

    std::int64_t MsSinceLastDialogue()
    {
        const auto last = g_lastDialogueMs.load(std::memory_order_relaxed);
        if (last == 0) {
            return -1;
        }
        return SteadyNowMs() - last;
    }

    bool SendCustomPromptToLLM(const std::string& promptName,
                               const std::string& variant,
                               const std::string& contextJson,
                               std::function<void(std::string, bool)> callback)
    {
        if (!g_available || !::PublicSendCustomPromptToLLM) {
            return false;
        }

        // SkyrimNet hands the response back as a `const char*` that is only
        // valid for the duration of its call, on one of its own worker
        // threads. Copy it into a std::string before handing control to our
        // caller; the caller is responsible for getting to the main thread
        // before touching anything in the game.
        auto adapted = [cb = std::move(callback)](const char* response, int success) {
            if (cb) {
                cb(response ? std::string{ response } : std::string{}, success != 0);
            }
        };

        try {
            return ::PublicSendCustomPromptToLLM(promptName.c_str(), variant.c_str(), contextJson.c_str(),
                                                 std::move(adapted));
        } catch (const std::exception& e) {
            logger::error("SkyrimNet: SendCustomPromptToLLM threw: {}", e.what());
            return false;
        }
    }
}
