#include "UI.h"

#include "Director.h"
#include "PendingImpulse.h"
#include "Settings.h"
#include "State.h"

#include <SKSEMenuFramework.h>

// SKSEMenuFramework's header exposes the ImGui surface as ImGuiMCP (every call
// forwards into SKSEMenuFramework.dll). Alias it so the render code below
// reads like ordinary ImGui.
namespace ImGui = ImGuiMCP;

namespace AgencyEngine::UI
{
    namespace
    {
        // One vocabulary for the two places delivery is named: the combo here
        // and the expiry choice on the same tab. If these strings drift the
        // page starts describing the same two behaviours in two ways.
        const char* const kDeliveryItems[] = {
            "Recorded, never voiced (private event)",
            "Spoken aloud (direct narration)",
        };

        constexpr ImGuiMCP::ImVec4 kGood{ 0.4f, 0.9f, 0.4f, 1.0f };
        constexpr ImGuiMCP::ImVec4 kWarn{ 0.85f, 0.8f, 0.45f, 1.0f };
        constexpr ImGuiMCP::ImVec4 kBad{ 0.95f, 0.4f, 0.4f, 1.0f };
        constexpr ImGuiMCP::ImVec4 kNote{ 0.95f, 0.55f, 0.3f, 1.0f };
        constexpr ImGuiMCP::ImVec4 kSpeaker{ 0.6f, 0.8f, 1.0f, 1.0f };

        // The page used to explain each setting in three to five hand-broken
        // Text lines, which put more documentation than control on screen and
        // made the widgets hard to find. Same words, moved behind a hover.
        void HelpMarker(const char* text)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", text);
            }
        }

        // A sentence that has to be visible without hovering — a hard
        // requirement or an escape hatch, not background.
        void Note(const char* text)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", text);
            ImGui::PopTextWrapPos();
        }

        // ---- Status ---------------------------------------------------------

        void __stdcall RenderStatus()
        {
            Status state;
            WithState([&](Status& live) { state = live; });
            const auto settings = SnapshotSettings();

            // One line that answers "what is it doing right now", before any
            // of the detail. Everything below exists to explain this line.
            ImGui::PushTextWrapPos(0.0f);
            if (!settings.enabled) {
                ImGui::TextColored(kWarn, "%s", "Disabled - no impulses will fire.");
            } else if (!state.skyrimNetAvailable) {
                ImGui::TextColored(kBad, "%s", "SkyrimNet not found - nothing can run.");
            } else if (!state.snapshot.valid) {
                ImGui::TextColored(kWarn, "%s", "Waiting for a loaded save.");
            } else if (state.inFlight) {
                ImGui::TextColored(kGood, "%s", "Generating an impulse now.");
            } else if (state.deliveryPending) {
                ImGui::TextColored(kWarn, "Written, waiting for a gap in the conversation (%.0f s of %.0f).",
                                   state.deliveryHeldSeconds, settings.maxDeferSeconds);
            } else if (!state.holdReason.empty()) {
                ImGui::TextColored(kWarn, "Holding: %s", state.holdReason.c_str());
            } else if (state.armed) {
                const double elapsedMinutes = (state.snapshot.gameDays - state.lastFireGameDays) * 24.0 * 60.0;
                const double remaining = settings.intervalGameMinutes - elapsedMinutes;
                ImGui::TextColored(kGood, "Running - next impulse in %.0f in-game minutes (interval %.0f).",
                                   remaining < 0.0 ? 0.0 : remaining, settings.intervalGameMinutes);
            } else {
                ImGui::TextColored(kGood, "%s", "Running - the countdown starts with the next tick.");
            }
            ImGui::PopTextWrapPos();

            // Both actions are things you reach for *because* of the line
            // above, so they belong next to it rather than past a scroll.
            if (ImGui::Button("Generate an impulse now")) {
                Director::RequestFireNow();
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart timer")) {
                Director::ResetTimer();
            }

            // ---- everything that can be wrong, in one place ----------------
            ImGui::SeparatorText("Health");
            if (state.skyrimNetAvailable) {
                ImGui::TextColored(kGood, "SkyrimNet connected (API v%d)", state.skyrimNetVersion);
            } else {
                ImGui::TextColored(kBad, "%s", "SkyrimNet not found - SkyrimNet.dll did not load.");
            }

            if (settings.deferOnConversation || settings.injectQuietGap) {
                if (!state.quiet.valid) {
                    ImGui::TextColored(kNote, "%s",
                                       "Conversation bridge silent - Scripts/AgencyEngine_Bridge.pex may be "
                                       "missing or stale.");
                } else {
                    ImGui::TextColored(kGood, "%s", "Conversation bridge reporting.");
                }
            }

            if (settings.combatContinuousMode && state.gameMasterOff) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(kNote, "%s",
                                   "Continuous mode toggle did nothing. SkyrimNet ignores it while the GameMaster "
                                   "agent is disabled - enable GameMaster in SkyrimNet.");
                ImGui::PopTextWrapPos();
            }

            if (!state.lastError.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(kNote, "Last error: %s", state.lastError.c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::SeparatorText("World");
            if (!state.snapshot.valid) {
                ImGui::Text("%s", "No save loaded.");
            } else {
                ImGui::Text("Player: %s", state.snapshot.playerName.c_str());
                ImGui::Text("Location: %s",
                            state.snapshot.location.empty() ? "(unknown)" : state.snapshot.location.c_str());
                ImGui::Text("In combat: %s", state.snapshot.playerInCombat ? "yes" : "no");
                ImGui::Text("Followers: %d", static_cast<int>(state.snapshot.followers.size()));
                for (const auto& follower : state.snapshot.followers) {
                    ImGui::BulletText("%s (%08X)", follower.name.c_str(), follower.formID);
                }
            }

            ImGui::SeparatorText("This session");
            ImGui::Text("%d spoken, %d quiet", state.impulsesThisSession, state.silencesThisSession);
            // The per-lens split lives on the Lenses settings tab now, next to
            // the weights it is evidence for.
            if (!state.lensTallies.empty()) {
                ImGui::TextDisabled("%s", "Per-lens split is on Settings > Lenses.");
            }

            // A count only. The list, and everything you can do to it, is its
            // own page — same arrangement as the per-lens tallies, which moved
            // next to the weights they are evidence for.
            if (settings.pendingBioInjection) {
                ImGui::SeparatorText("Carried, unsaid");
                if (state.pendingImpulses.empty()) {
                    ImGui::TextDisabled("%s", "Nobody is carrying anything.");
                } else {
                    ImGui::Text("%d companion(s) carrying something unsaid.",
                                static_cast<int>(state.pendingImpulses.size()));
                    ImGui::TextDisabled("%s", "The list is on the 'Carried' page.");
                }
            }

            if (settings.deferOnConversation || settings.injectQuietGap) {
                ImGui::SeparatorText("Conversation");
                if (!state.quiet.valid) {
                    ImGui::Text("%s", "No reading yet.");
                } else if (state.quiet.recording) {
                    ImGui::Text("%s", "Player is speaking into the microphone.");
                } else if (state.quiet.speechQueue > 0) {
                    ImGui::Text("Speech queue: %d line(s) pending.", state.quiet.speechQueue);
                } else {
                    ImGui::Text("Quiet for %.1f s (threshold %.0f s).", state.quiet.msSinceAudioEnded / 1000.0,
                                settings.quietSeconds);
                }
            }

            if (settings.combatContinuousMode) {
                ImGui::SeparatorText("Continuous mode");
                ImGui::Text("SkyrimNet continuous mode: %s%s", state.continuousEnabled ? "on" : "off",
                            state.continuousOwned ? " (held by us for this fight)" : "");
                if (state.continuousPending) {
                    ImGui::Text("%s", "Waiting for Papyrus to confirm the last change.");
                }
            }
        }

        // ---- Settings -------------------------------------------------------

        void RenderImpulsesTab(Settings& s, bool& dirty)
        {
            dirty |= ImGui::Checkbox("Enabled", &s.enabled);
            HelpMarker("Master switch. Off, the loop still ticks but never fires.");

            dirty |= ImGui::SliderFloat("Interval (in-game minutes, default 120)", &s.intervalGameMinutes, 5.0f,
                                        1440.0f, "%.0f");
            HelpMarker("How much in-game time passes between impulses. Measured in game time, so it\n"
                       "stretches and compresses with the timescale rather than with the clock.");
            ImGui::TextDisabled("= %.1f in-game hours (~%.0f real minutes at timescale 20).",
                                s.intervalGameMinutes / 60.0f, s.intervalGameMinutes / 20.0f);

            dirty |= ImGui::Checkbox("Only when a follower is present", &s.requireFollower);
            HelpMarker("This mod is about companions. With nobody following, there is nobody to speak up.");

            dirty |= ImGui::SliderInt("Force someone to speak (% of ticks, default 20)", &s.forcedImpulseChance, 0,
                                      100);
            HelpMarker("On a forced tick the prompt loses the silence option entirely and someone has to\n"
                       "speak up. The rest of the time the model may return silence, and normally will.\n"
                       "Applies to every lens.\n"
                       "Never forced while the party is mid-exchange - forcing a turn into a live scene is\n"
                       "where this prompt writes its worst output.");
            if (s.forcedImpulseChance <= 0) {
                ImGui::TextDisabled("%s", "Never forced - purely the model's judgement, and the quietest setting.");
            } else if (s.forcedImpulseChance >= 100) {
                ImGui::TextColored(kWarn, "%s",
                                   "Every quiet tick speaks - and it shows. A forced impulse on a thin day is the "
                                   "weakest thing this prompt writes.");
            }
            Note("The per-lens 'N spoken, M quiet' counters on the Status page are the readout: raise this if "
                 "it is all quiet, lower it if the companions start sounding scheduled.");

            dirty |= ImGui::Checkbox("Also generate a private thought (second LLM call)", &s.generateThought);
            HelpMarker("After an impulse is delivered, asks the speaker to think privately about what they\n"
                       "just raised. Costs a second LLM call, and is what lets the next impulse know this\n"
                       "one happened. Skipped when SkyrimNet's own thought cooldown is active.");

            ImGui::SeparatorText("Recorded impulses she goes on carrying");
            dirty |= ImGui::Checkbox("Keep a recorded impulse in her character bio", &s.pendingBioInjection);
            HelpMarker("A recorded impulse is one she never says out loud. With this on it is held by\n"
                       "AgencyEngine and written into her own prompt word for word, and into nobody\n"
                       "else's - so it colours what she says next without any LLM call to deliver it.\n"
                       "Off falls back to asking SkyrimNet to generate a thought from it instead, which\n"
                       "costs a call and paraphrases the text.");
            Note("Applies to the recorded delivery: whenever 'Delivery' on the Speaking up tab is set to "
                 "recorded, and whenever a spoken impulse gives up waiting for a gap.");

            ImGui::BeginDisabled(!s.pendingBioInjection);
            dirty |= ImGui::SliderFloat("Forget it after (in-game minutes, default 720)", &s.pendingTtlGameMinutes,
                                        30.0f, 4320.0f, "%.0f");
            HelpMarker("Something she has been meaning to raise for three in-game days is not an agenda,\n"
                       "it is a fixture. Past this it is dropped.");
            ImGui::TextDisabled("= %.1f in-game hours.", s.pendingTtlGameMinutes / 60.0f);

            dirty |= ImGui::SliderFloat("Check whether it is still live every (in-game minutes, 0 = never)",
                                        &s.pendingResolveGameMinutes, 0.0f, 720.0f, "%.0f");
            HelpMarker("Asks a cheap LLM call whether the subject has since been had out, and clears it if\n"
                       "so. Worth having: the usual way an agenda stops being live is that the\n"
                       "conversation covered it, which the timer above cannot see. Point it at a cheap\n"
                       "model under SkyrimNet's own settings - the variant is 'agencyengine_resolve'.");
            if (s.pendingResolveGameMinutes <= 0.0f) {
                ImGui::TextColored(kWarn, "%s", "Off - only the timer above will clear a recorded impulse.");
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText("Already raised");
            dirty |= ImGui::Checkbox("Remember what each companion has raised", &s.ledgerEnabled);
            HelpMarker("Shows the impulse prompt what each companion has already said out loud, so it\n"
                       "stops proposing the same subject. Without it the only record is SkyrimNet's\n"
                       "event tail, which is capped by count - so the evidence that a subject was raised\n"
                       "drains long before the state that produced it does, and it comes back.");

            ImGui::BeginDisabled(!s.ledgerEnabled);
            dirty |= ImGui::SliderInt("Subjects remembered per companion (default 6)", &s.ledgerSlots, 1, 20);
            HelpMarker("Oldest drops off when the list is full, and that subject can be raised again.\n"
                       "By count rather than by a clock on purpose: a quiet in-game week should not make\n"
                       "a settled subject fair game again, but six other things having come and gone is\n"
                       "a fair test of enough having changed.");

            dirty |= ImGui::Checkbox("Refuse an impulse that repeats one", &s.ledgerVeto);
            HelpMarker("A backstop for when the prompt is ignored. The call is already spent by then, so\n"
                       "what this buys is that she does not say it twice - and a log line naming the\n"
                       "subject that was held back, which is how you tell a prompt being ignored from\n"
                       "one that is working.");
            ImGui::EndDisabled();
        }

        void RenderSpeakingTab(Settings& s, bool& dirty)
        {
            dirty |= ImGui::Combo("Delivery", &s.delivery, kDeliveryItems,
                                  static_cast<int>(std::size(kDeliveryItems)));
            HelpMarker("Recorded: the impulse lands in the event history of the companion and whoever she\n"
                       "addressed - nobody else present reads it - but is never voiced. It colours what\n"
                       "she says next instead.\n"
                       "Spoken: the companion is handed a speaking turn on it. This is the point of the mod.");

            const bool spoken = s.delivery == kDirectNarration;
            if (!spoken) {
                Note("The rest of this tab is about not interrupting a conversation. A recorded impulse never "
                     "interrupts anything, so none of it applies.");
                return;
            }

            ImGui::SeparatorText("Don't interrupt a conversation");
            dirty |= ImGui::Checkbox("Wait for a gap before speaking up", &s.deferOnConversation);
            HelpMarker("An LLM request takes 4-8 seconds, so checking before sending it protects nothing -\n"
                       "the party can start talking while it is in flight. The impulse is written on\n"
                       "schedule and then held until nobody is recording voice input, the speech queue is\n"
                       "empty, and the silence below has elapsed.");

            ImGui::BeginDisabled(!s.deferOnConversation);
            dirty |= ImGui::SliderFloat("Silence required (seconds, default 25)", &s.quietSeconds, 0.0f, 60.0f,
                                        "%.0f");
            HelpMarker("Measured against four signals: nobody recording voice input, nothing in the speech\n"
                       "queue, this long since the last NPC audio ended, and this long since anyone took a\n"
                       "dialogue turn. The dialogue-turn clock is what makes the number mean anything - on\n"
                       "audio alone, you composing a line reads as total silence.");
            dirty |= ImGui::SliderFloat("Give up after (seconds, default 60)", &s.maxDeferSeconds, 5.0f, 300.0f,
                                        "%.0f");
            HelpMarker("How long a finished impulse waits for that silence before the choice below applies.");
            dirty |= ImGui::Checkbox("On giving up, record it instead of dropping it", &s.degradeToPersistentEvent);
            HelpMarker("Recorded, never voiced: the companion does not interrupt, and the topic simply lands\n"
                       "in her own context - and her target's - where it colours what she says next.\n"
                       "Unchecked, the impulse is discarded.");
            ImGui::EndDisabled();

            ImGui::SeparatorText("What the model is told about the gap");
            dirty |= ImGui::Checkbox("Tell the prompt how long the party has been quiet", &s.injectQuietGap);
            HelpMarker("Lets the model tell 'they stopped talking a moment ago' from 'nobody has spoken in\n"
                       "an hour' - which decides whether the last exchange is still off-limits. Costs\n"
                       "nothing once the reading exists, and is independent of waiting for a gap.");
        }

        void RenderCombatTab(Settings& s, bool& dirty)
        {
            dirty |= ImGui::Checkbox("Skip impulses while in combat", &s.skipInCombat);
            HelpMarker("An unprompted aside lands badly mid-fight. The countdown keeps running; the tick is\n"
                       "declined.");

            ImGui::SeparatorText("Hand the fight to SkyrimNet instead");
            dirty |= ImGui::Checkbox("Hold continuous mode for the length of a fight", &s.combatContinuousMode);
            HelpMarker("Switches SkyrimNet's continuous scene mode on when combat starts and back off when it\n"
                       "ends, so the party keeps talking through the fight.\n"
                       "If continuous mode was already on when combat started it is left alone, never\n"
                       "switched off. Loading a save mid-fight hands it back; quitting to desktop mid-fight\n"
                       "leaves it on, since it is SkyrimNet's setting rather than save data.");
            Note("Requires SkyrimNet's GameMaster agent to be enabled - the toggle does nothing without it, "
                 "and there is no way to ask, so a failed switch-on is reported on the Status page.");

            ImGui::BeginDisabled(!s.combatContinuousMode);
            dirty |= ImGui::SliderFloat("Grace before switching off (seconds, default 10)",
                                        &s.continuousExitGraceSeconds, 0.0f, 60.0f, "%.0f");
            HelpMarker("Combat drops briefly between waves. Waiting this long before switching off stops the\n"
                       "mode - and SkyrimNet's on-screen notification - flickering.");
            ImGui::EndDisabled();
        }

        void RenderLensesTab(Settings& s, bool& dirty, const std::vector<LensTally>& tallies)
        {
            Note("Every impulse is asked through a lens: a prompt that inherits "
                 "agencyengine_impulse_base.prompt and asks for one kind of thing. The chosen lens never "
                 "repeats twice running while another is available, so weights set the long-run mix rather "
                 "than each individual tick.");
            Note("Weight 0 disables a lens outright - which is also how you switch off a lens whose prompt "
                 "needs a mod you do not have installed.");

            int total = 0;
            for (const auto& lens : s.lenses) {
                if (lens.prompt[0] != '\0') {
                    total += lens.weight > 0 ? lens.weight : 0;
                }
            }

            if (ImGui::BeginTable("lenses", 5,
                                  ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
                                      ImGuiMCP::ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("Prompt file", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn("Weight", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Share", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.5f);
                ImGui::TableSetupColumn("This session", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < kMaxLenses; ++i) {
                    auto& lens = s.lenses[i];
                    // Unique ImGui IDs per row: without the PushID every row's
                    // widgets share a label, and typing in one writes to
                    // whichever row drew first.
                    ImGui::PushID(i);
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    dirty |= ImGui::InputText("##name", lens.name, sizeof(lens.name));

                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    dirty |= ImGui::InputText("##prompt", lens.prompt, sizeof(lens.prompt));

                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    dirty |= ImGui::SliderInt("##weight", &lens.weight, 0, 100);

                    // Weights only mean anything relative to each other, and
                    // the arithmetic is exactly what someone gets wrong when
                    // they bump one row from 50 to 60.
                    ImGui::TableNextColumn();
                    if (lens.prompt[0] == '\0' || lens.weight <= 0) {
                        ImGui::TextDisabled("%s", "off");
                    } else {
                        ImGui::Text("%.0f%%", total > 0 ? 100.0 * lens.weight / total : 0.0);
                    }

                    // A lens that is always quiet is either asking for
                    // something this playthrough doesn't have yet or asking for
                    // something the state cannot evidence — and the ratio is
                    // the only way to tell those apart from a mixed history.
                    ImGui::TableNextColumn();
                    const auto tally = std::find_if(tallies.begin(), tallies.end(),
                                                    [&](const LensTally& t) { return t.name == lens.name; });
                    if (tally == tallies.end()) {
                        ImGui::TextDisabled("%s", "-");
                    } else {
                        ImGui::Text("%d spoken, %d quiet", tally->spoken, tally->quiet);
                    }

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            HelpMarker("Each prompt file resolves to Data/SKSE/Plugins/SkyrimNet/prompts/<name>.prompt");
            if (total <= 0) {
                ImGui::TextColored(kWarn, "%s",
                                   "Every lens is weighted 0 - there is nothing to ask, so no impulse fires.");
            }
        }

        void RenderContextTab(Settings& s, bool& dirty)
        {
            dirty |= ImGui::SliderInt("Player events (default 40)", &s.maxEvents, 5, 200);
            HelpMarker("How many of SkyrimNet's recent events for the player are fed to the prompt.");
            dirty |= ImGui::SliderInt("Thoughts per follower (default 10)", &s.perFollowerEvents, 0, 120);
            HelpMarker("A per-follower recent-event tail, on top of the player's.\n"
                       "The prompt renders only the events carrying a thought and drops the rest, so\n"
                       "unfiltered this is an event budget spent as a thought budget - after a fight,\n"
                       "ten events can render none, and 40 is what it takes to reliably get any.\n"
                       "With the follower filter below set to the thought type, every one renders and\n"
                       "this means what it says - so drop it to about 10, or a full party puts several\n"
                       "thousand tokens of interior monologue in every prompt.");
            if (s.followerEventTypeFilter[0] != '\0' && s.perFollowerEvents > 20) {
                ImGui::TextColored(kWarn, "%s",
                                   "Filtered AND high: that is this many thoughts per follower, every tick.");
            }
            dirty |= ImGui::InputText("Event type filter", s.eventTypeFilter, sizeof(s.eventTypeFilter));
            HelpMarker("Comma-separated SkyrimNet event types; empty = every type.");
            dirty |= ImGui::InputText("Event type filter (followers)", s.followerEventTypeFilter,
                                      sizeof(s.followerEventTypeFilter));
            HelpMarker("The same, for the per-follower tail only; empty falls back to the filter above.\n"
                       "Defaults to npc_thoughts, SkyrimNet's type for a private NPC thought, which is\n"
                       "what makes the count above mean thoughts. If a SkyrimNet update renames it the\n"
                       "symptom is the thought section going empty - clear this and raise the count.");
        }

        void RenderDiagnosticsTab(Settings& s, bool& dirty)
        {
            ImGui::BeginDisabled(!s.deferOnConversation && !s.injectQuietGap);
            dirty |= ImGui::SliderFloat("Conversation poll (seconds, default 1)", &s.quietPollSeconds, 0.25f, 5.0f,
                                        "%.2f");
            HelpMarker("How often the Papyrus bridge is asked whether anyone is talking. One static call per\n"
                       "interval, running continuously while either conversation feature is on.");
            ImGui::EndDisabled();

            dirty |= ImGui::Checkbox("Verbose tick logging", &s.debugLog);
            HelpMarker("Adds per-tick snapshot lines and full context/response payloads to\n"
                       "Documents/My Games/Skyrim Special Edition/SKSE/AgencyEngine.log.\n"
                       "The Director ticks once a second, so this is the difference between a log you can\n"
                       "read and one you have to grep.");

            ImGui::SeparatorText("Configuration file");
            ImGui::TextDisabled("%s", Settings::FilePath().string().c_str());
            if (ImGui::Button("Reload from disk")) {
                // Load() takes no lock of its own; the caller already holds
                // g_settingsLock for the whole tab.
                s.Load();
                dirty = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset to defaults")) {
                s = Settings{};
                dirty = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "(reset is not written until you save)");
        }

        void __stdcall RenderSettings()
        {
            // Latched across frames, not frame-local. ImGui widgets return true
            // only on the frame a value actually changes, so a local flag makes
            // the "unsaved changes" note appear for one frame and disappear —
            // which reads as the note never appearing, and then as the Save
            // button doing nothing. Render thread only, so a static is safe.
            static bool                                  dirty = false;
            static std::chrono::steady_clock::time_point savedAt{};

            // Copied before g_settingsLock is taken: two locks, never nested.
            std::vector<LensTally> tallies;
            WithState([&](Status& state) { tallies = state.lensTallies; });

            {
                std::scoped_lock lock{ g_settingsLock };
                auto& s = g_settings;

                if (ImGui::BeginTabBar("agencyengine_settings")) {
                    if (ImGui::BeginTabItem("Impulses")) {
                        RenderImpulsesTab(s, dirty);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Speaking up")) {
                        RenderSpeakingTab(s, dirty);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Combat")) {
                        RenderCombatTab(s, dirty);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Lenses")) {
                        RenderLensesTab(s, dirty, tallies);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Context")) {
                        RenderContextTab(s, dirty);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Diagnostics")) {
                        RenderDiagnosticsTab(s, dirty);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }

            // Below the tab bar, so it is the same control whichever tab is up.
            ImGui::Separator();
            if (ImGui::Button("Save settings")) {
                const auto copy = SnapshotSettings();
                if (copy.Save()) {
                    dirty = false;
                    savedAt = std::chrono::steady_clock::now();
                } else {
                    // Save() already logged the reason; say *something* here,
                    // because a button that silently fails is worse than one
                    // that silently succeeds.
                    savedAt = {};
                }
            }
            ImGui::SameLine();
            if (dirty) {
                ImGui::TextColored(kWarn, "%s", "unsaved changes");
            } else if (std::chrono::steady_clock::now() - savedAt < 4s) {
                ImGui::TextColored(kGood, "%s", "saved to Data/SKSE/Plugins/AgencyEngine.json");
            } else {
                ImGui::Text("%s", "(changes apply immediately; saving keeps them for next session)");
            }
        }

        // ---- Carried, unsaid ------------------------------------------------
        //
        // What each companion is privately carrying, and the only place it is
        // visible at all: a pending impulse is rendered into her own prompt and
        // nowhere else, so without this page the only evidence it exists is the
        // log line that recorded it and the one that cleared it.
        //
        // It is also the page you reach for when tuning, which is why every
        // clearing condition is spelled out per row rather than left to the
        // log — "why did that vanish" and "why is that still there" are the two
        // questions this feature generates.

        // What each companion has already raised, and whether the slot is still
        // waiting on a verdict. Grouped by speaker rather than listed flat,
        // because "is this one person fixating" is the question anyone opens
        // this panel to answer.
        void RenderLedger()
        {
            const auto slots = PendingImpulses::LedgerSnapshot();

            ImGui::SeparatorText("Already raised");
            if (slots.empty()) {
                ImGui::TextDisabled("%s", "Nothing yet. Subjects land here once a companion says one out loud.");
                return;
            }

            const auto    settings = SnapshotSettings();
            std::uint32_t current = 0;
            for (const auto& slot : slots) {
                if (slot.formID != current) {
                    current = slot.formID;
                    // How full this character's list is, which is what decides
                    // when the oldest subject becomes fair game again.
                    const auto held = std::ranges::count_if(
                        slots, [&](const PendingImpulses::LedgerSlot& s) { return s.formID == current; });
                    ImGui::TextColored(kSpeaker, "%s", slot.speakerName.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d of %d", static_cast<int>(held), settings.ledgerSlots);
                }
                ImGui::BulletText("%s", slot.topic.c_str());
                if (slot.provisional) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", "(waiting to see if it was answered)");
                }
            }
            ImGui::TextDisabled("%s", "Oldest drops off when a companion's list is full, and can be raised again. "
                                      "One never answered is released as soon as she stops carrying it.");
        }

        void __stdcall RenderPending()
        {
            std::vector<PendingImpulses::Entry> pending;
            GameSnapshot                        snapshot;
            WithState([&](Status& state) {
                pending = state.pendingImpulses;
                snapshot = state.snapshot;
            });
            const auto settings = SnapshotSettings();

            // Counted before the early exits below, because the ledger and the
            // spoken entries both outlive "nothing is being carried" and either
            // one is a reason to keep drawing.
            std::size_t carried = 0;
            std::size_t said = 0;
            for (const auto& entry : pending) {
                (entry.spoken ? said : carried) += 1;
            }

            if (!settings.pendingBioInjection) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(kWarn, "%s",
                                   "'Keep a recorded impulse in her character bio' is off, so nothing new will be "
                                   "carried unsaid. Anything already said out loud is still tracked below, and so "
                                   "is the ledger.");
                ImGui::PopTextWrapPos();
            }

            // Deliberately not an early return when pending is empty: the ledger
            // is the record that outlives every entry, so it is exactly when
            // nobody is carrying anything that it is the only thing worth
            // showing.
            if (pending.empty()) {
                ImGui::Text("%s", "Nothing is open right now - nobody is carrying anything unsaid, and nothing "
                                  "said is still waiting on an answer.");
                Note("An entry appears here either when an impulse is recorded rather than spoken, or when one is "
                     "spoken and we start watching whether anyone picks it up.");
                RenderLedger();
                return;
            }

            ImGui::Text("%zu not said yet, %zu said and unanswered", carried, said);
            Note("Not said yet: she is waiting for an opening, and it is in her bio. Said and unanswered: she "
                 "raised it, nobody met it, and it is holding a place in the ledger until we know which way it "
                 "went.");

            const auto queued = Director::PendingResolveRequests();
            if (ImGui::Button("Check all now")) {
                for (const auto& entry : pending) {
                    Director::RequestResolveCheck(entry.formID);
                }
            }
            HelpMarker("Asks the LLM, for each one, whether the subject has since been had out - and clears the\n"
                       "ones that have. One call per companion, run one at a time rather than all at once.\n"
                       "Works even with the scheduled check switched off.");
            if (queued > 0) {
                ImGui::SameLine();
                ImGui::TextColored(kWarn, "%d check(s) queued", static_cast<int>(queued));
            }

            for (const auto& entry : pending) {
                ImGui::PushID(static_cast<int>(entry.formID));
                ImGui::SeparatorText(entry.speakerName.c_str());

                ImGui::TextColored(kSpeaker, "%s -> %s", entry.speakerName.c_str(), entry.targetName.c_str());
                ImGui::SameLine();
                // The state, said plainly and in colour, because every other
                // line on the row means something different depending on it.
                if (entry.spoken) {
                    ImGui::TextColored(kWarn, "%s", "[said, unanswered]");
                } else {
                    ImGui::TextColored(kNote, "%s", "[not said yet]");
                }

                // The ledger's key, and the one field whose quality decides
                // whether any of this works — two impulses about one subject
                // have to produce near-identical slugs or nothing matches.
                if (!entry.topic.empty()) {
                    ImGui::TextDisabled("subject: %s", entry.topic.c_str());
                } else {
                    ImGui::TextColored(kWarn, "%s",
                                       "no subject recorded - nothing will stop this being raised again");
                }

                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("%s", entry.text.c_str());
                ImGui::PopTextWrapPos();

                if (entry.unverified) {
                    // Restored from the sidecar and not yet matched against a
                    // live actor. Withheld from the prompt until it is, so say
                    // so rather than showing it as active.
                    ImGui::TextColored(kNote, "%s", "restoring from the last session - not in her prompt yet");
                }

                // Everything that will eventually end this entry, in one place.
                // The age counts from whichever phase it is in: speaking restarts
                // the TTL, so counting from creation would show a countdown that
                // does not match when it actually expires.
                const double anchor = entry.spoken ? entry.spokenGameDays : entry.createdGameDays;
                const double ageMinutes = (snapshot.gameDays - anchor) * 24.0 * 60.0;
                const double ttlLeft = settings.pendingTtlGameMinutes - ageMinutes;
                ImGui::TextDisabled("%s for %.0f in-game minutes%s", entry.spoken ? "unanswered" : "carried",
                                    ageMinutes < 0.0 ? 0.0 : ageMinutes,
                                    entry.lens.empty() ? "" : std::format("  |  {} question", entry.lens).c_str());
                if (settings.pendingTtlGameMinutes > 0.0f) {
                    ImGui::TextDisabled("forgotten in %.0f in-game minutes", ttlLeft < 0.0 ? 0.0 : ttlLeft);
                }
                if (settings.pendingResolveGameMinutes > 0.0f) {
                    const double sinceCheck = (snapshot.gameDays - entry.lastCheckGameDays) * 24.0 * 60.0;
                    const double nextCheck = settings.pendingResolveGameMinutes - sinceCheck;
                    ImGui::TextDisabled("next 'still live?' check in %.0f in-game minutes",
                                        nextCheck < 0.0 ? 0.0 : nextCheck);
                } else {
                    ImGui::TextDisabled("%s", "'still live?' checks are off - only the timer above will clear it");
                }

                // Which way each exit sends the subject. This is the part of the
                // design that is invisible from anywhere else: two entries that
                // look identical here end up opposite ways depending on which
                // clock or check reaches them first.
                if (entry.spoken && !entry.topic.empty()) {
                    ImGui::TextDisabled("%s", "if the check says it was answered -> stays off the table");
                    ImGui::TextDisabled("%s", "if it runs out the clock unanswered -> she can raise it again");
                } else if (!entry.spoken) {
                    ImGui::TextDisabled("%s",
                                        "never said aloud, so either way it costs her nothing - "
                                        "the subject stays available");
                }

                if (ImGui::Button("Check now")) {
                    Director::RequestResolveCheck(entry.formID);
                }
                HelpMarker("One LLM call: has she had this out since it was recorded? Cleared if yes, left alone\n"
                           "if no or if the answer cannot be read.");
                ImGui::SameLine();
                if (ImGui::Button("Forget this")) {
                    // Takes PendingImpulses' own lock, not the Status one, so
                    // this is safe from the render thread. The Status mirror is
                    // rebuilt on the Director's next tick; erase it here too so
                    // the row goes away on this frame rather than in a second.
                    PendingImpulses::Clear(entry.formID, "cleared by hand from the UI");
                    const auto formID = entry.formID;
                    WithState([&](Status& state) {
                        std::erase_if(state.pendingImpulses,
                                      [&](const PendingImpulses::Entry& e) { return e.formID == formID; });
                    });
                }
                HelpMarker("Drops it immediately. She stops carrying it and it leaves her bio; nothing else\n"
                           "about her changes. The next impulse can give her a new one.");

                ImGui::PopID();
            }

            ImGui::Separator();
            if (ImGui::Button("Forget all")) {
                for (const auto& entry : pending) {
                    PendingImpulses::Clear(entry.formID, "cleared by hand from the UI (forget all)");
                }
                WithState([](Status& state) { state.pendingImpulses.clear(); });
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "(clears every row above)");

            RenderLedger();
        }

        // ---- History --------------------------------------------------------

        void __stdcall RenderHistory()
        {
            std::deque<Impulse> history;
            std::string lastContext;
            WithState([&](Status& state) {
                history = state.history;
                lastContext = state.lastContextJson;
            });

            if (history.empty()) {
                ImGui::Text("%s", "No impulses generated yet.");
            }

            for (const auto& impulse : history) {
                ImGui::SeparatorText(impulse.when.c_str());
                if (!impulse.ok) {
                    ImGui::TextColored(kBad, "%s", "(delivery failed)");
                }
                if (!impulse.speaker.empty()) {
                    ImGui::TextColored(kSpeaker, "%s -> %s", impulse.speaker.c_str(), impulse.target.c_str());
                }
                if (!impulse.topic.empty()) {
                    // Shown per row because this is where you read whether the
                    // model names a subject the same way twice. If it does not,
                    // the ledger cannot match a repeat and none of the rest
                    // works — and this list is the only place that is visible.
                    ImGui::TextDisabled("subject: %s", impulse.topic.c_str());
                }
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("%s", impulse.content.c_str());
                ImGui::PopTextWrapPos();
                if (impulse.lens.empty()) {
                    ImGui::TextDisabled("via %s", impulse.delivery.c_str());
                } else {
                    ImGui::TextDisabled("via %s  |  %s question", impulse.delivery.c_str(), impulse.lens.c_str());
                }
            }

            if (!lastContext.empty() && ImGui::CollapsingHeader("Last context sent to the model")) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("%s", lastContext.c_str());
                ImGui::PopTextWrapPos();
            }
        }
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::warn("SKSEMenuFramework not installed — no in-game UI. Everything else still runs.");
            return;
        }

        SKSEMenuFramework::SetSection("Agency Engine");
        SKSEMenuFramework::AddSectionItem("Status", RenderStatus);
        SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
        SKSEMenuFramework::AddSectionItem("Carried", RenderPending);
        SKSEMenuFramework::AddSectionItem("History", RenderHistory);

        WithState([](Status& state) { state.menuFrameworkPresent = true; });
        logger::info("UI: registered under 'Agency Engine'");
    }
}
