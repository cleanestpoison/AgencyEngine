#include "PapyrusBridge.h"

#include "Logging.h"
#include "State.h"

#include <nlohmann/json.hpp>

namespace AgencyEngine::PapyrusBridge
{
    namespace
    {
        constexpr auto kScript = "SkyrimNetApi"sv;
        // Ours, not SkyrimNet's — see statics/Scripts/AgencyEngine_Bridge.pex.
        constexpr auto kOwnScript = "AgencyEngine_Bridge"sv;
        constexpr auto kContinuousEvent = "AgencyEngine_ContinuousMode"sv;
        constexpr auto kQuietEvent = "AgencyEngine_Quiet"sv;

        // SkyrimNet's ByUUID natives take the UUID as a Papyrus String and
        // treat "" as "unspecified".
        //
        // The string must be UPPERCASE HEX — that is what SkyrimNetApi.psc
        // documents GetEntityUUID as returning, and what the ByUUID natives
        // parse. Sending decimal here does not fail loudly: the originator
        // simply does not resolve, SkyrimNet falls back to "unspecified", and
        // DirectNarration then picks its own speaker. The symptom is a
        // completely different NPC saying our line.
        RE::BSFixedString UuidArg(std::uint64_t uuid)
        {
            return uuid ? RE::BSFixedString{ std::format("{:X}", uuid) } : RE::BSFixedString{ "" };
        }

        bool Dispatch3(std::string_view function,
                       const std::string& content,
                       std::uint64_t originatorUuid,
                       std::uint64_t targetUuid)
        {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                logger::error("PapyrusBridge: the Papyrus VM singleton is null — cannot dispatch {}.{}", kScript,
                              function);
                return false;
            }

            // Info rather than debug, and the UUID strings verbatim: when the
            // wrong NPC speaks, the originator string is the first and only
            // thing worth looking at.
            logger::info("PapyrusBridge: dispatching {}.{}(content={} chars, originator='{}', target='{}')",
                         kScript, function, content.size(), UuidArg(originatorUuid).c_str(),
                         UuidArg(targetUuid).c_str());

            RE::BSFixedString a0{ content };
            RE::BSFixedString a1 = UuidArg(originatorUuid);
            RE::BSFixedString a2 = UuidArg(targetUuid);

            // MakeFunctionArguments heap-allocates; DispatchStaticCall does not
            // take ownership, so we delete it ourselves once the call returns.
            auto* args = RE::MakeFunctionArguments(std::move(a0), std::move(a1), std::move(a2));
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;

            const bool ok = vm->DispatchStaticCall(RE::BSFixedString{ kScript.data() },
                                                   RE::BSFixedString{ function.data() }, args, result);
            delete args;

            if (ok) {
                logger::debug("PapyrusBridge: {}.{} dispatched", kScript, function);
            } else {
                // The VM refuses the call when the script or the function isn't
                // registered — i.e. SkyrimNet's natives aren't there. Say so,
                // because the alternative reading ("the impulse was rejected") would
                // send someone looking in entirely the wrong place.
                logger::error("PapyrusBridge: DispatchStaticCall {}.{} was refused by the VM. The script or "
                              "function is not registered — check that SkyrimNet is installed and loaded.",
                              kScript, function);
            }
            return ok;
        }

        // RegisterEventByUUID takes the event type ahead of the content, so it
        // is Dispatch3 plus one leading string. Kept separate rather than
        // generalised: the argument order is the whole content of both
        // functions, and a variadic version would hide it.
        bool Dispatch4(std::string_view function,
                       const std::string& eventType,
                       const std::string& content,
                       std::uint64_t originatorUuid,
                       std::uint64_t targetUuid)
        {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                logger::error("PapyrusBridge: the Papyrus VM singleton is null — cannot dispatch {}.{}", kScript,
                              function);
                return false;
            }

            logger::info("PapyrusBridge: dispatching {}.{}(type='{}', content={} chars, originator='{}', "
                         "target='{}')",
                         kScript, function, eventType, content.size(), UuidArg(originatorUuid).c_str(),
                         UuidArg(targetUuid).c_str());

            RE::BSFixedString a0{ eventType };
            RE::BSFixedString a1{ content };
            RE::BSFixedString a2 = UuidArg(originatorUuid);
            RE::BSFixedString a3 = UuidArg(targetUuid);

            auto* args = RE::MakeFunctionArguments(std::move(a0), std::move(a1), std::move(a2), std::move(a3));
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;

            const bool ok = vm->DispatchStaticCall(RE::BSFixedString{ kScript.data() },
                                                   RE::BSFixedString{ function.data() }, args, result);
            delete args;

            if (!ok) {
                logger::error("PapyrusBridge: DispatchStaticCall {}.{} was refused by the VM. The script or "
                              "function is not registered — check that SkyrimNet is installed and loaded.",
                              kScript, function);
            }
            return ok;
        }

        // --- the event schema -------------------------------------------------
        //
        // Field type codes are SkyrimNet's: 0=String, 1=Integer, 2=Boolean,
        // 3=Double, 4=Object, 5=Array. Only `kind` and `msg` are required, so a
        // future kind can leave everything else empty without failing
        // validation.
        constexpr auto kSchemaFields = R"([
{"name":"kind","type":0,"required":true,"description":"What this event is: 'impulse' for something a companion chose to raise unprompted. One event type covers everything AgencyEngine records; this field says which."},
{"name":"msg","type":0,"required":true,"description":"The prose the NPC reads - for an impulse, the stage direction naming the topic and why it surfaces now."},
{"name":"speaker","type":0,"required":false,"description":"Display name of the companion the event belongs to."},
{"name":"target","type":0,"required":false,"description":"Display name of whoever it was addressed to, usually the player."},
{"name":"lens","type":0,"required":false,"description":"Which impulse lens produced it (aspiration, relationship, or empty for the general question)."},
{"name":"detail","type":0,"required":false,"description":"Free-form extra context for kinds that need it. Empty for impulses."}
])";

        // components/event_history.prompt renders "verbose" and prefixes the
        // speaker's name itself ("Lydia (to you): ..."), so verbose must not
        // repeat the names — it only wraps the line in asterisks, the same way
        // that template treats direct narration. "recent_events" is written for
        // completeness; with shortLivedEnabled off, nothing should ever reach
        // scene context to render it.
        constexpr auto kSchemaTemplates = R"({
"recent_events":"*{{msg}}*",
"raw":"{{msg}}",
"compact":"{{msg}}",
"verbose":"*{{msg}}*"
})";

        // Set once RegisterEventSchemas has dispatched. Read by RecordEvent to
        // decide whether writing against kEventType is safe yet.
        std::atomic_bool g_schemaReady{ false };
    }

    bool RegisterEventSchemas()
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            logger::error("PapyrusBridge: the Papyrus VM singleton is null — cannot register the event schema");
            return false;
        }

        RE::BSFixedString eventType{ kEventType.data() };
        RE::BSFixedString displayName{ "AgencyEngine Event" };
        RE::BSFixedString description{ "Something a companion did on their own initiative - an impulse they chose "
                                       "to raise unprompted, and whatever else AgencyEngine records later." };
        RE::BSFixedString fields{ kSchemaFields };
        RE::BSFixedString templates{ kSchemaTemplates };
        // isEphemeral false + TTL 0: these are history, not scene decoration.
        bool         isEphemeral = false;
        std::int32_t defaultTTLMs = 0;
        // The entire point. True here (SkyrimNet's default) would put a copy in
        // scene context, where every NPC present reads it regardless of who the
        // event names.
        bool shortLivedEnabled = false;
        // False: an impulse must not cut into speech that is already playing.
        // The Director has its own conversation-aware hold for that decision.
        bool interrupt = false;

        // Defaults on a Papyrus native are filled in by the *compiler*, so a
        // call made from here has to supply all nine arguments itself.
        auto* args = RE::MakeFunctionArguments(std::move(eventType), std::move(displayName), std::move(description),
                                               std::move(fields), std::move(templates), std::move(isEphemeral),
                                               std::move(defaultTTLMs), std::move(shortLivedEnabled),
                                               std::move(interrupt));
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;

        const bool ok = vm->DispatchStaticCall(RE::BSFixedString{ kScript.data() },
                                               RE::BSFixedString{ "RegisterEventSchema" }, args, result);
        delete args;

        if (ok) {
            g_schemaReady.store(true);
            logger::info("PapyrusBridge: registered the '{}' event schema (scene-context copies off, so an event is "
                         "read only by the actors named on it)",
                         kEventType);
        } else {
            // Not fatal — RecordEvent falls back — but it means every recorded
            // impulse is public again, which is exactly the bug this schema
            // exists to fix.
            logger::error("PapyrusBridge: DispatchStaticCall {}.RegisterEventSchema was refused by the VM. Recorded "
                          "impulses will fall back to persistent events, which every nearby NPC can read.",
                          kScript);
            WithState([](Status& state) {
                state.lastError = "the AgencyEngine event schema could not be registered — recorded impulses are "
                                  "visible to every nearby NPC";
            });
        }
        return ok;
    }

    bool RecordEvent(const EventRecord& record)
    {
        if (record.msg.empty()) {
            logger::warn("PapyrusBridge: refusing to record an empty '{}' event", record.kind);
            return false;
        }

        if (!g_schemaReady.load()) {
            // Logged every time rather than once: each line here is an impulse
            // that leaked to the whole party, and the count matters.
            logger::warn("PapyrusBridge: the '{}' schema is not registered yet — recording this {} as a persistent "
                         "event instead, which every nearby NPC can read",
                         kEventType, record.kind);
            return RegisterPersistentEvent(record.msg, record.speakerUuid, record.targetUuid);
        }

        // Every key is present even when empty, so a format template never
        // dereferences a key the data object lacks.
        const nlohmann::json payload{
            { "kind", std::string{ record.kind } }, { "msg", record.msg },   { "speaker", record.speaker },
            { "target", record.target },            { "lens", record.lens }, { "detail", record.detail },
        };

        return Dispatch4("RegisterEventByUUID"sv, std::string{ kEventType }, payload.dump(), record.speakerUuid,
                         record.targetUuid);
    }

    bool RegisterPersistentEvent(const std::string& content, std::uint64_t originatorUuid, std::uint64_t targetUuid)
    {
        if (content.empty()) {
            // SkyrimNet rejects empty content for this call outright.
            logger::warn("PapyrusBridge: refusing to register an empty persistent event");
            return false;
        }
        return Dispatch3("RegisterPersistentEventByUUID"sv, content, originatorUuid, targetUuid);
    }

    bool DirectNarration(const std::string& content, std::uint64_t originatorUuid, std::uint64_t targetUuid)
    {
        return Dispatch3("DirectNarrationByUUID"sv, content, originatorUuid, targetUuid);
    }

    bool GenerateNPCThought(RE::Actor* actor, const std::string& promptHint)
    {
        if (!actor) {
            logger::warn("PapyrusBridge: no actor to think — skipping GenerateNPCThought");
            return false;
        }
        if (promptHint.empty()) {
            logger::warn("PapyrusBridge: refusing to ask for a thought with no prompt hint");
            return false;
        }

        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            logger::error("PapyrusBridge: the Papyrus VM singleton is null — cannot dispatch {}.GenerateNPCThought",
                          kScript);
            return false;
        }

        logger::debug("PapyrusBridge: dispatching {}.GenerateNPCThought(actor='{}', hint={} chars)", kScript,
                      actor->GetDisplayFullName(), promptHint.size());

        // Unlike the event calls this one is Actor-typed — SkyrimNet ships no
        // ByUUID overload. CommonLibSSE packs the form pointer into a VM handle
        // for us, so it is still a plain MakeFunctionArguments call.
        auto* args = RE::MakeFunctionArguments(std::move(actor), RE::BSFixedString{ promptHint });
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;

        const bool ok = vm->DispatchStaticCall(RE::BSFixedString{ kScript.data() },
                                               RE::BSFixedString{ "GenerateNPCThought" }, args, result);
        delete args;

        if (!ok) {
            logger::error("PapyrusBridge: DispatchStaticCall {}.GenerateNPCThought was refused by the VM. The "
                          "script or function is not registered — check that SkyrimNet is installed and loaded.",
                          kScript);
        }
        return ok;
    }

    bool SetContinuousMode(bool desired)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            logger::error("PapyrusBridge: the Papyrus VM singleton is null — cannot set continuous mode");
            return false;
        }

        logger::info("PapyrusBridge: asking Papyrus to turn continuous mode {}", desired ? "ON" : "OFF");

        auto* args = RE::MakeFunctionArguments(std::move(desired));
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;

        const bool ok = vm->DispatchStaticCall(RE::BSFixedString{ kOwnScript.data() },
                                               RE::BSFixedString{ "SetContinuousMode" }, args, result);
        delete args;

        if (!ok) {
            // Distinct from the SkyrimNet case: this script is *ours*, so a
            // refusal means our own .pex didn't reach the Data folder — a
            // packaging problem, not a missing dependency.
            logger::error("PapyrusBridge: DispatchStaticCall {}.SetContinuousMode was refused by the VM. "
                          "Scripts/{}.pex is missing from the AgencyEngine mod folder, or is being overridden.",
                          kOwnScript, kOwnScript);
            WithState([](Status& state) {
                state.lastError = "AgencyEngine_Bridge.pex is missing — combat continuous mode cannot work";
                state.continuousPending = false;
            });
        }
        return ok;
    }

    bool PollQuiet()
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return false;
        }

        // No log line on the happy path: this runs about once a second while an
        // impulse is pending, and a per-poll trace would bury everything else.
        // Zero-argument call, but MakeFunctionArguments still heap-allocates and
        // DispatchStaticCall still doesn't take ownership.
        auto* args = RE::MakeFunctionArguments();
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
        const bool ok = vm->DispatchStaticCall(RE::BSFixedString{ kOwnScript.data() },
                                               RE::BSFixedString{ "PollQuiet" }, args, result);
        delete args;

        if (!ok) {
            // Logged on change only, by the caller — a refused poll repeats
            // every tick and would otherwise fill the file.
            static bool complained = false;
            if (!complained) {
                complained = true;
                logger::error("PapyrusBridge: DispatchStaticCall {}.PollQuiet was refused by the VM. "
                              "Scripts/{}.pex is missing or out of date — conversation-aware delivery is off, so "
                              "impulses will be spoken whenever they arrive.",
                              kOwnScript, kOwnScript);
                WithState([](Status& state) {
                    state.lastError = "AgencyEngine_Bridge.pex is missing or stale — cannot detect conversations";
                });
            }
        }
        return ok;
    }

    namespace
    {
        // The Papyrus helper's only way to answer. Fires on whichever thread ran
        // the script — main or VM — so it touches nothing but Status and the log.
        class ContinuousModeSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent*        a_event,
                                                  RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
            {
                if (!a_event || a_event->eventName != kContinuousEvent.data()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Bitfield rather than two events, so the caller sees one
                // consistent before/after pair. See AgencyEngine_Bridge.psc.
                const auto bits = static_cast<int>(a_event->numArg);
                const bool before = (bits & 1) != 0;
                const bool after = (bits & 2) != 0;
                const bool acquire = a_event->strArg == "acquire";

                // We own the mode only when we are the ones who switched it on.
                // Finding it already on means the player turned it on for their
                // own reasons, and switching it off after the fight would be us
                // undoing a decision that was never ours.
                const bool owned = acquire && !before && after;
                // The toggle is a no-op while SkyrimNet's GameMaster agent is
                // disabled, and SkyrimNet exposes no way to ask about the agent
                // directly — this is the only way we find out.
                const bool gameMasterOff = acquire && !after;

                bool announceGameMaster = false;
                WithState([&](Status& state) {
                    state.continuousEnabled = after;
                    state.continuousPending = false;
                    state.continuousReports += 1;
                    if (acquire) {
                        state.continuousOwned = owned;
                    } else {
                        state.continuousOwned = false;
                    }
                    announceGameMaster = gameMasterOff != state.gameMasterOff;
                    state.gameMasterOff = gameMasterOff;
                });

                if (announceGameMaster && gameMasterOff) {
                    logger::warn("Continuous mode did not switch on. SkyrimNet ignores the continuous-mode toggle "
                                 "while the GameMaster agent is disabled — enable GameMaster in SkyrimNet, or turn "
                                 "off 'Continuous mode during combat' here.");
                } else if (acquire && before) {
                    logger::info("Continuous mode was already on when combat started — leaving it alone, and it "
                                 "will not be switched off when combat ends");
                } else if (acquire && owned) {
                    logger::info("Continuous mode switched ON for combat (we now owe a switch-off)");
                } else if (!acquire) {
                    logger::info("Continuous mode switched {} after combat", after ? "but is somehow still ON" : "OFF");
                }

                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterContinuousModeSink()
    {
        static ContinuousModeSink sink;
        static bool               registered = false;
        if (registered) {
            return;
        }

        auto* source = SKSE::GetModCallbackEventSource();
        if (!source) {
            logger::error("PapyrusBridge: no mod callback event source — continuous mode results will not be "
                          "reported back");
            return;
        }
        source->AddEventSink(&sink);
        registered = true;
        logger::info("PapyrusBridge: listening for {}", kContinuousEvent);
    }

    namespace
    {
        class QuietSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent*        a_event,
                                                  RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
            {
                if (!a_event || a_event->eventName != kQuietEvent.data()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                // strArg is "<recording 0|1>;<speech queue size>" — see
                // AgencyEngine_Bridge.psc. Parsed defensively: a malformed
                // reading must read as "someone might be talking", never as
                // silence, because silence is what lets us interrupt.
                QuietReading reading;
                reading.msSinceAudioEnded = static_cast<int>(a_event->numArg);

                const std::string packed{ a_event->strArg.c_str() };
                const auto        sep = packed.find(';');
                if (sep == std::string::npos) {
                    logger::warn("PapyrusBridge: malformed quiet reading '{}' — treating as busy", packed);
                    return RE::BSEventNotifyControl::kContinue;
                }
                reading.recording = packed.substr(0, sep) == "1";
                try {
                    reading.speechQueue = std::stoi(packed.substr(sep + 1));
                } catch (const std::exception&) {
                    logger::warn("PapyrusBridge: unparsable speech queue in '{}' — treating as busy", packed);
                    return RE::BSEventNotifyControl::kContinue;
                }
                reading.valid = true;
                reading.receivedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count();

                WithState([&](Status& state) { state.quiet = reading; });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterQuietSink()
    {
        static QuietSink sink;
        static bool      registered = false;
        if (registered) {
            return;
        }

        auto* source = SKSE::GetModCallbackEventSource();
        if (!source) {
            logger::error("PapyrusBridge: no mod callback event source — conversation detection will not work");
            return;
        }
        source->AddEventSink(&sink);
        registered = true;
        logger::info("PapyrusBridge: listening for {}", kQuietEvent);
    }
}
