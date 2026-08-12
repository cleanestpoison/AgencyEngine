#include "Director.h"
#include "Logging.h"
#include "PapyrusBridge.h"
#include "PendingImpulse.h"
#include "Settings.h"
#include "SkyrimNetAPI.h"
#include "State.h"
#include "UI.h"

namespace AgencyEngine
{
    namespace
    {
        // Is a plugin in the load order? TESDataHandler is populated by
        // kDataLoaded, so at every call site below this is a plain synchronous
        // lookup with no ordering caveat.
        bool IsPluginLoaded(std::string_view name)
        {
            // Non-const: LookupModByName is not a const member in CommonLibSSE.
            auto* handler = RE::TESDataHandler::GetSingleton();
            return handler && handler->LookupModByName(name) != nullptr;
        }

        // The relationship lens' base prompt calls sever_player_blurb and
        // sever_companion_opinions, two decorators supplied by SeverActions.
        // Inja resolves decorator names when it *parses* a template, so on an
        // install without SeverActions those names are a parse error that costs
        // the entire impulse — not an empty section in one prompt. That is also
        // why no template flag can guard them: the failure happens before any
        // {% if %} is evaluated.
        //
        // So make the dependency optional from this side instead. Registering
        // inert stand-ins under the same names when SeverActions is absent lets
        // the template parse, and the prompt already branches on "" and falls
        // back to "nothing settled yet" — the lens runs, with thinner material.
        //
        // Keyed on the plugin rather than on asking SkyrimNet whether the
        // decorator exists: SeverActions registers its own from Papyrus when a
        // save loads, long after kDataLoaded. A presence check here would be
        // false even where it is installed, and the stand-in would then shadow
        // the real thing for the rest of the session.
        void RegisterRelationshipFallbacks()
        {
            if (IsPluginLoaded("SeverActions.esp"sv)) {
                logger::info("SeverActions is in the load order; leaving its relationship decorators to it");
                return;
            }

            for (const auto* name : { "sever_player_blurb", "sever_companion_opinions" }) {
                SkyrimNetAPI::RegisterDecorator(
                    name,
                    "AgencyEngine: inert stand-in for the SeverActions decorator of this name, which is not "
                    "installed. Always returns an empty string.",
                    [](RE::Actor*) -> std::string { return {}; });
            }
            logger::info("SeverActions is not installed; registered inert stand-ins for its two relationship "
                         "decorators. The relationship lens will run without standing data.");
        }

        void OnMessage(SKSE::MessagingInterface::Message* message)
        {
            if (!message) {
                return;
            }

            switch (message->type) {
            case SKSE::MessagingInterface::kPostLoad:
                logger::info("SKSE: kPostLoad");
                break;

            case SKSE::MessagingInterface::kPostPostLoad:
                logger::info("SKSE: kPostPostLoad");
                break;

            case SKSE::MessagingInterface::kInputLoaded:
                logger::info("SKSE: kInputLoaded");
                break;

            case SKSE::MessagingInterface::kSaveGame:
                logger::debug("SKSE: kSaveGame");
                break;

            case SKSE::MessagingInterface::kDataLoaded:
                {
                    logger::info("SKSE: kDataLoaded — initialising");
                    // SkyrimNet.dll is guaranteed loaded by now, so its exports
                    // can be resolved. Everything below is safe without a save.
                    const bool available = SkyrimNetAPI::Initialize();
                    const int version = SkyrimNetAPI::GetVersion();
                    WithState([&](Status& state) {
                        state.skyrimNetAvailable = available;
                        state.skyrimNetVersion = version;
                    });

                    {
                        std::scoped_lock lock{ g_settingsLock };
                        g_settings.Load();
                    }

                    // The Papyrus helper answers by mod event, so the sink has
                    // to be up before the Director can ask it anything.
                    PapyrusBridge::RegisterContinuousModeSink();
                    PapyrusBridge::RegisterQuietSink();
                    // Push-based, unlike the Papyrus poll: SkyrimNet calls us
                    // on every dialogue event. This is the only signal that
                    // sees the player's half of a conversation.
                    if (available) {
                        SkyrimNetAPI::StartDialogueClock();

                        // The only channel that reaches one NPC's prompt and
                        // nobody else's. Registered here, once, because
                        // submodules/character_bio/7200_pending_impulse.prompt
                        // names it — and Inja resolves decorators when it
                        // *parses* a file, so an unregistered name is a parse
                        // error that costs every prompt rendering that bio, not
                        // just an empty string in one of them.
                        SkyrimNetAPI::RegisterDecorator(
                            PendingImpulses::kDecoratorName,
                            "AgencyEngine: everything this companion has been meaning to raise and has not said out "
                            "loud yet, newest first, one per line as a markdown list item — or an empty string when "
                            "there is nothing.",
                            // SkyrimNet's thread, mid-render. Reads a FormID and
                            // builds one string — no other RE:: calls, nothing
                            // that can block on the main thread.
                            [](RE::Actor* actor) -> std::string {
                                if (!actor) {
                                    return {};
                                }
                                return PendingImpulses::Get(actor->GetFormID());
                            });

                        // Same block, two wordings, and a companion can need
                        // both at once — one lens's subject unsaid while
                        // another's waits on an answer. Telling her the second
                        // is the first — "you have not said it out loud" about a
                        // thing she just said — would have her raise it again on
                        // the next line, so they are rendered as separate
                        // sections from separate decorators rather than switched
                        // between. Registered alongside the text one because the
                        // bio prompt names all three, and an unregistered name is
                        // a parse error for the whole file.
                        SkyrimNetAPI::RegisterDecorator(
                            PendingImpulses::kSpokenDecoratorName,
                            "AgencyEngine: everything this companion has raised out loud and had no answer to, "
                            "newest first, one per line as a markdown list item — or an empty string when there is "
                            "nothing.",
                            [](RE::Actor* actor) -> std::string {
                                if (!actor) {
                                    return {};
                                }
                                return PendingImpulses::GetSpoken(actor->GetFormID());
                            });

                        // Nothing this build ships calls this one. It stays
                        // registered for an install whose prompt files are older
                        // than its DLL: Inja resolves decorator names at parse
                        // time, so an unregistered name costs that install its
                        // whole character bio rather than one block of it.
                        SkyrimNetAPI::RegisterDecorator(
                            PendingImpulses::kStateDecoratorName,
                            "AgencyEngine: compatibility only — 'carried' when this companion is carrying anything "
                            "unsaid, 'spoken' when the only thing open is something she has raised, empty when there "
                            "is nothing.",
                            [](RE::Actor* actor) -> std::string {
                                if (!actor) {
                                    return {};
                                }
                                return PendingImpulses::State(actor->GetFormID());
                            });

                        RegisterRelationshipFallbacks();
                    }
                    UI::Register();
                    Director::Start();
                    break;
                }

            case SKSE::MessagingInterface::kNewGame:
            case SKSE::MessagingInterface::kPostLoadGame:
                // Game time jumps on a load; restart the countdown so an impulse
                // doesn't fire the instant the save comes up.
                logger::info("SKSE: {} — restarting the impulse timer",
                             message->type == SKSE::MessagingInterface::kNewGame ? "kNewGame" : "kPostLoadGame");
                Director::ResetTimer();
                // Whatever was pending belongs to the save we just left. Drop it
                // and forget which save it came from; the Director's next tick
                // reloads whatever this save has, once SkyrimNet can name it.
                PendingImpulses::Reset();
                // SkyrimNet keeps event schemas in the DLL, not in the save, so
                // they are gone on every load and have to be put back. This
                // message arrives on the main thread with the VM up, which is
                // all the dispatch needs.
                PapyrusBridge::RegisterEventSchemas();
                break;

            default:
                break;
            }
        }
    }

    SKSEPluginLoad(const SKSE::LoadInterface* skse)
    {
        SKSE::Init(skse);
        SetupLog();

        // The header block every bug report needs: our version, the SKSE we
        // were loaded by, and the runtime we're running against.
        const auto* declaration = SKSE::PluginDeclaration::GetSingleton();
        logger::info("=== AgencyEngine {} ===", declaration->GetVersion().string());
        logger::info("SKSE runtime version {}, plugin handle {}", skse->RuntimeVersion().string(),
                     static_cast<std::uint32_t>(skse->GetPluginHandle()));
        logger::info("Log level is trace; per-tick verbose lines are gated on the 'Verbose tick logging' setting");

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(OnMessage)) {
            logger::error("Failed to register the SKSE message listener");
            return false;
        }

        return true;
    }
}
