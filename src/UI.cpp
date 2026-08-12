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
                ImGui::TextColored(kWarn, "Carried - a cue is waiting for a gap in the conversation (%.0f s of "
                                           "%.0f).",
                                   state.deliveryHeldSeconds, settings.maxDeferSeconds);
            } else if (!state.holdReason.empty()) {
                ImGui::TextColored(kWarn, "Holding: %s", state.holdReason.c_str());
            } else if (state.lensClocks.empty()) {
                ImGui::TextColored(kGood, "%s", "Running - the lens clocks arm on the next pass.");
            } else {
                ImGui::TextColored(kGood, "%s", "Running - every lens is counting down on its own clock.");
            }
            ImGui::PopTextWrapPos();

            // Each lens's own countdown. There is no shared interval any more,
            // so "when does the next thing happen" has as many answers as there
            // are lenses, and one number here would be a fiction.
            for (const auto& clock : state.lensClocks) {
                const double minutes = (clock.dueGameDays - state.snapshot.gameDays) * 24.0 * 60.0;
                if (clock.inFlight) {
                    ImGui::TextDisabled("    %s - asking now", clock.name.c_str());
                } else {
                    ImGui::TextDisabled("    %s - asks in %.0f in-game minutes", clock.name.c_str(),
                                        minutes < 0.0 ? 0.0 : minutes);
                }
            }

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
            ImGui::Text("%d carried, %d quiet", state.impulsesThisSession, state.silencesThisSession);
            // The per-lens split lives on the Lenses settings tab now, next to
            // the cadence it is evidence for.
            if (!state.lensTallies.empty()) {
                ImGui::TextDisabled("%s", "Per-lens split is on Settings > Lenses.");
            }

            // A count only. The list, and everything you can do to it, is its
            // own page — same arrangement as the per-lens tallies, which moved
            // next to the cadence they are evidence for.
            if (settings.pendingBioInjection) {
                ImGui::SeparatorText("Carried, unsaid");
                if (state.pendingImpulses.empty()) {
                    ImGui::TextDisabled("%s", "Nobody is carrying anything.");
                } else {
                    // Impulses, not companions: one companion can be carrying
                    // one from each lens, and the number people watch while
                    // tuning is how many subjects are open at once.
                    ImGui::Text("%d impulse(s) open across the party.",
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
            HelpMarker("Master switch. Off, the loop still passes but no lens is ever asked.");
            Note("How often each lens is asked is on the Lenses tab - every lens has its own interval and its "
                 "own cooldown, and there is no shared one behind them.");

            dirty |= ImGui::Checkbox("Only when a follower is present", &s.requireFollower);
            HelpMarker("This mod is about companions. With nobody following, there is nobody to speak up.");

            dirty |= ImGui::SliderInt("Force someone to speak (% of asks, default 20)", &s.forcedImpulseChance, 0,
                                      100);
            HelpMarker("On a forced ask the prompt loses the silence option entirely and someone has to\n"
                       "speak up. The rest of the time the model may return silence, and normally will.\n"
                       "Applies to every lens, and is rolled per ask - so several lenses coming due together\n"
                       "roll it several times.\n"
                       "Never forced while the party is mid-exchange - forcing a turn into a live scene is\n"
                       "where this prompt writes its worst output.");
            if (s.forcedImpulseChance <= 0) {
                ImGui::TextDisabled("%s", "Never forced - purely the model's judgement, and the quietest setting.");
            } else if (s.forcedImpulseChance >= 100) {
                ImGui::TextColored(kWarn, "%s",
                                   "Every ask speaks - and it shows. A forced impulse on a thin day is the "
                                   "weakest thing this prompt writes.");
            }
            Note("The per-lens 'N carried, M quiet' counters on the Lenses tab are the readout: raise this if "
                 "it is all quiet, lower it if the companions start sounding scheduled.");

            dirty |= ImGui::Checkbox("Also generate a private thought (second LLM call)", &s.generateThought);
            HelpMarker("After an impulse is delivered, asks the speaker to think privately about what they\n"
                       "just raised. Costs a second LLM call, and is what lets the next impulse know this\n"
                       "one happened. Skipped when SkyrimNet's own thought cooldown is active.");

            ImGui::SeparatorText("What she goes on carrying");
            dirty |= ImGui::Checkbox("Hold the impulse in her character bio", &s.pendingBioInjection);
            HelpMarker("This is how an impulse is delivered. It is held by AgencyEngine and written into her\n"
                       "own prompt word for word, and into nobody else's - so it is there to colour what she\n"
                       "says without any LLM call to deliver it, and the cue on the Speaking up tab only has\n"
                       "to announce that there is something there.\n"
                       "Off falls back to asking SkyrimNet to generate a thought from it instead, which costs\n"
                       "a call, paraphrases the text, and leaves nothing for a cue to be about - so no cue is\n"
                       "sent either. Kept as an A/B against the carried version.");
            Note("One impulse per companion per lens, so she can be carrying an aspiration and a proposal at "
                 "once. Her bio renders them newest first.");

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
                ImGui::TextColored(kWarn, "%s", "Off - only the timer above will clear a carried impulse, and "
                                                "a cue never waits on a check.");
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
                       "a fair test of enough having changed.\n"
                       "Counted per lens: each lens forgets only its own subjects. This is the number a\n"
                       "lens uses when its own Slots column on the Lenses tab is left at 'shared'.");

            dirty |= ImGui::Checkbox("Refuse an impulse that repeats one", &s.ledgerVeto);
            HelpMarker("A backstop for when the prompt is ignored. The call is already spent by then, so\n"
                       "what this buys is that she does not say it twice - and a log line naming the\n"
                       "subject that was held back, which is how you tell a prompt being ignored from\n"
                       "one that is working.");
            ImGui::EndDisabled();
        }

        void RenderSpeakingTab(Settings& s, bool& dirty)
        {
            Note("An impulse always lands in her bio first, whatever this tab says. What is on this tab is "
                 "the cue: a vague line - she has something on her mind - that hands her a speaking turn "
                 "without naming a subject, because her bio is already carrying it.");

            dirty |= ImGui::Checkbox("Announce a fresh impulse with a cue", &s.cues);
            HelpMarker("One pending cue per companion, however many impulses she picks up - which is why the\n"
                       "sentence is vague: a cue that named a subject would be false the moment a second one\n"
                       "joined it.\n"
                       "Off is pure drift. Nothing is narrated at all, and what she is carrying surfaces only\n"
                       "as it colours what she says once somebody else starts talking. That costs the timing,\n"
                       "not the agenda.");

            if (!s.cues) {
                Note("The rest of this tab is about when a cue goes out. With cues off, nothing does.");
                return;
            }

            ImGui::SeparatorText("Don't interrupt a conversation");
            dirty |= ImGui::Checkbox("Wait for a gap before cueing her", &s.deferOnConversation);
            HelpMarker("An LLM request takes 4-8 seconds, so checking before sending it protects nothing -\n"
                       "the party can start talking while it is in flight. So the impulse is asked for on\n"
                       "schedule and carried straight away, and only the cue waits: until nobody is recording\n"
                       "voice input, the speech queue is empty, and the silence below has elapsed.");

            ImGui::BeginDisabled(!s.deferOnConversation);
            dirty |= ImGui::SliderFloat("Silence required (seconds, default 25)", &s.quietSeconds, 0.0f, 60.0f,
                                        "%.0f");
            HelpMarker("Measured against four signals: nobody recording voice input, nothing in the speech\n"
                       "queue, this long since the last NPC audio ended, and this long since anyone took a\n"
                       "dialogue turn. The dialogue-turn clock is what makes the number mean anything - on\n"
                       "audio alone, you composing a line reads as total silence.");
            dirty |= ImGui::SliderFloat("Give up after (seconds, default 60)", &s.maxDeferSeconds, 5.0f, 300.0f,
                                        "%.0f");
            HelpMarker("How long a cue waits for that silence before it is dropped. Dropping it costs the\n"
                       "opening and nothing else - she is still carrying the subject, and it still colours\n"
                       "what she says.");
            ImGui::EndDisabled();

            Note("If the party talked while a cue was waiting, the 'still live?' check runs on what she is "
                 "carrying before the cue goes out, and the cue is dropped if that exchange settled all of "
                 "it. That is what stops her announcing a subject the party just closed.");

            ImGui::SeparatorText("What the model is told about the gap");
            dirty |= ImGui::Checkbox("Tell the prompt how long the party has been quiet", &s.injectQuietGap);
            HelpMarker("Lets the model tell 'they stopped talking a moment ago' from 'nobody has spoken in\n"
                       "an hour' - which decides whether the last exchange is still off-limits. Costs\n"
                       "nothing once the reading exists, and is independent of waiting for a gap.");
        }

        void RenderCombatTab(Settings& s, bool& dirty)
        {
            dirty |= ImGui::Checkbox("Skip impulses while in combat", &s.skipInCombat);
            HelpMarker("An unprompted aside lands badly mid-fight. Every lens clock keeps running; the ask is\n"
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

        void RenderLensesTab(Settings& s, bool& dirty, const std::vector<LensTally>& tallies,
                             const std::vector<LensClock>& clocks, double nowGameDays)
        {
            Note("Every impulse is asked through a lens: a prompt that inherits "
                 "agencyengine_impulse_base.prompt and asks for one kind of thing. Each lens asks on its own "
                 "clock - they do not take turns and they do not compete, because asking two questions is "
                 "not worse than asking one.");
            Note("Interval is how long a lens waits between asks, and it is both the chattiness knob and the "
                 "cost knob: an ask that comes back silent - which most do, by design - is still an LLM call. "
                 "Cooldown is the extra silence a lens takes once its question has actually landed as "
                 "something she is carrying, whether or not she has said it yet. That is what stops a lens "
                 "nagging: it cannot re-raise inside its cooldown, because it is never asked.");
            Note("Unticking a lens switches it off outright - which is also how you switch off a lens whose "
                 "prompt needs a mod you do not have installed.");
            Note("The mod's own lenses are shown greyed: their name, prompt file and kind come from the mod, so "
                 "an update can add one or fix one and you keep your cadence. Only the switch, the two clocks "
                 "and the slot count are yours, and only those are written to the config file. The blank rows "
                 "at the bottom are for a prompt you wrote yourself.");

            if (ImGui::BeginTable("lenses", 9,
                                  ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
                                      ImGuiMCP::ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Ask", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.3f);
                ImGui::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("Prompt file", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("Interval", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Cooldown", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Next ask", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.8f);
                ImGui::TableSetupColumn("Proposals", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.5f);
                ImGui::TableSetupColumn("Slots", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableSetupColumn("This session", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableHeadersRow();

                for (int i = 0; i < kMaxLenses; ++i) {
                    auto& lens = s.lenses[i];
                    // A row the mod ships. Its name and prompt file describe a
                    // file in the archive, so they are shown rather than edited —
                    // the old page let both be typed over, and the only thing
                    // that could do was point a lens at a prompt that doesn't
                    // resolve, which costs the whole impulse and looks like a
                    // lens that is simply always quiet.
                    const bool shipped = BuiltinLensFor(lens.id) != nullptr;

                    // Unique ImGui IDs per row: without the PushID every row's
                    // widgets share a label, and typing in one writes to
                    // whichever row drew first.
                    ImGui::PushID(i);
                    ImGui::TableNextRow();

                    // The whole of "does this lens run". No weight behind it:
                    // lenses no longer compete for a turn, so the only question
                    // left about one is whether it is asked at all.
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(lens.prompt[0] == '\0');
                    dirty |= ImGui::Checkbox("##enabled", &lens.enabled);
                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    if (shipped) {
                        ImGui::TextDisabled("%s", lens.name);
                    } else {
                        ImGui::SetNextItemWidth(-1.0f);
                        dirty |= ImGui::InputText("##name", lens.name, sizeof(lens.name));
                    }

                    ImGui::TableNextColumn();
                    if (shipped) {
                        ImGui::TextDisabled("%s", lens.prompt);
                    } else {
                        ImGui::SetNextItemWidth(-1.0f);
                        dirty |= ImGui::InputText("##prompt", lens.prompt, sizeof(lens.prompt));
                    }

                    // In-game minutes, like every other clock in this mod, but
                    // shown in hours underneath: nobody thinks about a
                    // relationship lens in units of 1440 minutes.
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    dirty |= ImGui::SliderFloat("##interval", &lens.intervalGameMinutes, 15.0f, 2880.0f,
                                                "%.0f min");
                    ImGui::TextDisabled("%.1f h", lens.intervalGameMinutes / 60.0f);

                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    dirty |= ImGui::SliderFloat("##cooldown", &lens.cooldownGameMinutes, 0.0f, 5760.0f,
                                                lens.cooldownGameMinutes <= 0.0f ? "none" : "%.0f min");
                    ImGui::TextDisabled("%.1f h", lens.cooldownGameMinutes / 60.0f);

                    // Read straight off the Director's clock, because a cadence
                    // you cannot watch running is a cadence nobody can tune. A
                    // row with no clock is one that is switched off, or one the
                    // Director has not passed over yet.
                    ImGui::TableNextColumn();
                    const auto clock = std::find_if(clocks.begin(), clocks.end(), [&](const LensClock& c) {
                        return c.key == (lens.id[0] != '\0' ? std::string_view{ lens.id }
                                                            : std::string_view{ lens.prompt });
                    });
                    if (lens.prompt[0] == '\0') {
                        ImGui::TextDisabled("%s", "-");
                    } else if (clock == clocks.end()) {
                        ImGui::TextDisabled("%s", lens.enabled ? "arming" : "off");
                    } else if (clock->inFlight) {
                        ImGui::TextColored(kGood, "%s", "asking now");
                    } else {
                        const double minutes = (clock->dueGameDays - nowGameDays) * 24.0 * 60.0;
                        ImGui::Text("%.0f min", minutes < 0.0 ? 0.0 : minutes);
                    }

                    // Declared, never inferred from the name above — which is a
                    // label the user is free to edit, and a rename that
                    // silently changed how an impulse resolves would fail as
                    // wrong behaviour rather than as an error. For a shipped
                    // lens it is declared by the prompt file's own text, so it
                    // is read-only here for the same reason the prompt is.
                    ImGui::TableNextColumn();
                    if (shipped) {
                        ImGui::TextDisabled("%s", lens.proposal ? "yes" : "no");
                    } else {
                        dirty |= ImGui::Checkbox("##proposal", &lens.proposal);
                    }

                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    dirty |= ImGui::SliderInt("##slots", &lens.ledgerSlots, 0, 20,
                                              lens.ledgerSlots == 0 ? "shared" : "%d");

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
                        ImGui::Text("%d carried, %d quiet", tally->carried, tally->quiet);
                    }

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            HelpMarker("Each prompt file resolves to Data/SKSE/Plugins/SkyrimNet/prompts/<name>.prompt");
            Note("Next ask counts down in in-game minutes, so it stretches and compresses with the timescale "
                 "rather than with the clock. Several lenses coming due at once is ordinary and simply "
                 "produces several asks.");
            Note("Proposals: tick this for a lens that asks the party to DO something together - a drink, a "
                 "round of sparring, a game. Agreement does not settle a proposal; only the thing happening, or "
                 "a plain refusal, does. 'Sure' followed by nothing is a deferral, and she keeps carrying it.");
            Note("Slots: how many subjects this lens remembers per companion, or 'shared' to use the number on "
                 "the Impulses tab. Each lens forgets only its own subjects, so a lens that cycles fast cannot "
                 "release what another one settled. A lens drawing on a small vocabulary - activities, where "
                 "there are perhaps ten - wants a small number, or it holds its whole repertoire and goes "
                 "quiet.");
            const bool anyEnabled = std::any_of(std::begin(s.lenses), std::end(s.lenses), [](const Lens& lens) {
                return lens.enabled && lens.prompt[0] != '\0';
            });
            if (!anyEnabled) {
                ImGui::TextColored(kWarn, "%s",
                                   "Every lens is switched off - there is nothing to ask, so no impulse fires.");
            }

            // Worth a button rather than "delete the config file": the file
            // holds the rest of the settings too, and someone who has spent an
            // evening on the cadence is exactly who wants it back without
            // losing their event filters.
            if (ImGui::Button("Restore the mod's lens defaults")) {
                for (int i = 0; i < kMaxLenses; ++i) {
                    const auto* builtin = BuiltinLensFor(s.lenses[i].id);
                    if (builtin) {
                        s.lenses[i].enabled = builtin->enabled;
                        s.lenses[i].intervalGameMinutes = builtin->intervalGameMinutes;
                        s.lenses[i].cooldownGameMinutes = builtin->cooldownGameMinutes;
                        s.lenses[i].ledgerSlots = builtin->ledgerSlots;
                    }
                }
                dirty = true;
            }
            HelpMarker("Puts every switch, interval, cooldown and slot count back to what this version of the\n"
                       "mod ships. Lenses you added yourself are left alone.");
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
                                   "Filtered AND high: that is this many thoughts per follower, every ask.");
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

            dirty |= ImGui::Checkbox("Verbose pass logging", &s.debugLog);
            HelpMarker("Adds per-pass snapshot lines and full context/response payloads to\n"
                       "Documents/My Games/Skyrim Special Edition/SKSE/AgencyEngine.log.\n"
                       "The Director passes once a second, so this is the difference between a log you can\n"
                       "read and one you have to grep. The two previous logs are kept alongside it as\n"
                       "AgencyEngine.log.1 and AgencyEngine.log.2.");

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
            std::vector<LensClock> clocks;
            double                 nowGameDays = 0.0;
            WithState([&](Status& state) {
                tallies = state.lensTallies;
                clocks = state.lensClocks;
                nowGameDays = state.snapshot.gameDays;
            });

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
                        RenderLensesTab(s, dirty, tallies, clocks, nowGameDays);
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

            const auto settings = SnapshotSettings();

            // How full a ring is, which is what decides when its oldest subject
            // becomes fair game again. Per ring rather than per character: the
            // cap is per lens now, so one number against the global setting
            // would read as full when no ring is.
            //
            // A slot whose lens is no longer configured — a renamed or deleted
            // row, or a slot older than the rings — belongs to the shared ring,
            // exactly as the ledger itself treats it.
            const auto capFor = [&](const std::string& lens) {
                for (const auto& configured : settings.lenses) {
                    if (configured.name[0] != '\0' && lens == configured.name) {
                        return configured.ledgerSlots > 0 ? configured.ledgerSlots : settings.ledgerSlots;
                    }
                }
                return settings.ledgerSlots;
            };
            const auto ringOf = [&](const std::string& lens) -> std::string {
                for (const auto& configured : settings.lenses) {
                    if (configured.name[0] != '\0' && lens == configured.name) {
                        return lens;
                    }
                }
                return {};
            };

            // Grouped by speaker, then by ring. Walked by first appearance
            // rather than assuming the snapshot is already grouped — it is one
            // flat list in insertion order, and two companions raising things
            // in turn interleave in it.
            std::vector<std::uint32_t> speakers;
            for (const auto& slot : slots) {
                if (std::ranges::find(speakers, slot.formID) == speakers.end()) {
                    speakers.push_back(slot.formID);
                }
            }

            for (const auto formID : speakers) {
                std::vector<std::string> rings;
                for (const auto& slot : slots) {
                    if (slot.formID != formID) {
                        continue;
                    }
                    auto ring = ringOf(slot.lens);
                    if (std::ranges::find(rings, ring) == rings.end()) {
                        rings.push_back(std::move(ring));
                    }
                }

                for (const auto& slot : slots) {
                    if (slot.formID == formID) {
                        ImGui::TextColored(kSpeaker, "%s", slot.speakerName.c_str());
                        break;
                    }
                }

                for (const auto& ring : rings) {
                    const auto held = std::ranges::count_if(slots, [&](const PendingImpulses::LedgerSlot& s) {
                        return s.formID == formID && ringOf(s.lens) == ring;
                    });
                    ImGui::TextDisabled("    %s  %d of %d",
                                        ring.empty() ? "shared (lenses that no longer exist)" : ring.c_str(),
                                        static_cast<int>(held), capFor(ring));

                    for (const auto& slot : slots) {
                        if (slot.formID != formID || ringOf(slot.lens) != ring) {
                            continue;
                        }
                        ImGui::BulletText("%s", slot.topic.c_str());
                        if (slot.provisional) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("%s", "(waiting to see if it was answered)");
                        }
                    }
                }
            }

            ImGui::TextDisabled("%s", "Each lens forgets only its own subjects: the oldest drops off when that "
                                      "lens's list is full, and can be raised again. One never answered is "
                                      "released as soon as she stops carrying it. The shared list holds anything "
                                      "raised before lenses had their own, and anything raised by a lens that has "
                                      "since been renamed or removed.");
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
                                   "'Hold the impulse in her character bio' is off, so nothing new will be "
                                   "carried and no cue will be sent. Anything already open is still tracked "
                                   "below, and so is the ledger.");
                ImGui::PopTextWrapPos();
            }

            // Deliberately not an early return when pending is empty: the ledger
            // is the record that outlives every entry, so it is exactly when
            // nobody is carrying anything that it is the only thing worth
            // showing.
            if (pending.empty()) {
                ImGui::Text("%s", "Nothing is open right now - nobody is carrying anything unsaid, and nothing "
                                  "said is still waiting on an answer.");
                Note("An entry appears here the moment a lens produces an impulse, and leaves when the 'still "
                     "live?' check says the subject was met or the timer forgets it.");
                RenderLedger();
                return;
            }

            ImGui::Text("%zu carried, %zu raised and unanswered", carried, said);
            Note("Carried: it is in her bio, and a cue hands her the turn to open it when the party goes quiet. "
                 "Raised and unanswered is a state from an older version of this mod, kept because entries from "
                 "it survive an upgrade - this build cannot tell which of the subjects she is carrying she "
                 "actually raised, so the 'still live?' check is what decides them all.");

            const auto queued = Director::PendingResolveRequests();
            if (ImGui::Button("Check all now")) {
                for (const auto& entry : pending) {
                    Director::RequestResolveCheck(entry.formID, entry.lens);
                }
            }
            HelpMarker("Asks the LLM, for each one, whether the subject has since been had out - and clears the\n"
                       "ones that have. One call per impulse rather than per companion, because each is its own\n"
                       "question, and they run one at a time rather than all at once. Works even with the\n"
                       "scheduled check switched off.");
            if (queued > 0) {
                ImGui::SameLine();
                ImGui::TextColored(kWarn, "%d check(s) queued", static_cast<int>(queued));
            }

            // One row per impulse, and a companion can occupy several: the ID
            // is the row index rather than the FormID, or two of her rows would
            // share every widget on them.
            for (int row = 0; row < static_cast<int>(pending.size()); ++row) {
                const auto& entry = pending[static_cast<std::size_t>(row)];
                ImGui::PushID(row);
                ImGui::SeparatorText(entry.lens.empty()
                                         ? entry.speakerName.c_str()
                                         : std::format("{}  |  {} question", entry.speakerName, entry.lens).c_str());

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
                // The lens is on the row's header already, so this is only the
                // clock.
                ImGui::TextDisabled("%s for %.0f in-game minutes", entry.spoken ? "unanswered" : "carried",
                                    ageMinutes < 0.0 ? 0.0 : ageMinutes);
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
                    Director::RequestResolveCheck(entry.formID, entry.lens);
                }
                HelpMarker("One LLM call: has she had this out since it was recorded? Cleared if yes, left alone\n"
                           "if no or if the answer cannot be read.");
                ImGui::SameLine();
                if (ImGui::Button("Forget this")) {
                    // Takes PendingImpulses' own lock, not the Status one, so
                    // this is safe from the render thread. The Status mirror is
                    // rebuilt on the Director's next pass; erase it here too so
                    // the row goes away on this frame rather than in a second.
                    // This row only — anything else she is carrying came from
                    // another lens and is nobody's business but its own.
                    PendingImpulses::Clear(entry.formID, entry.lens, "cleared by hand from the UI");
                    const auto formID = entry.formID;
                    const auto lens = entry.lens;
                    WithState([&](Status& state) {
                        std::erase_if(state.pendingImpulses, [&](const PendingImpulses::Entry& e) {
                            return e.formID == formID && e.lens == lens;
                        });
                    });
                }
                HelpMarker("Drops it immediately. She stops carrying it and it leaves her bio; nothing else\n"
                           "about her changes. The next impulse can give her a new one.");

                ImGui::PopID();
            }

            ImGui::Separator();
            if (ImGui::Button("Forget all")) {
                for (const auto& entry : pending) {
                    PendingImpulses::Clear(entry.formID, entry.lens, "cleared by hand from the UI (forget all)");
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
