#include "Director.h"
#include "CombatEpisode.h"

#include "Logging.h"
#include "PapyrusBridge.h"
#include "PendingImpulse.h"
#include "ResolutionScheduler.h"
#include "Settings.h"
#include "SkyrimNetAPI.h"
#include "State.h"

#include <nlohmann/json.hpp>

namespace AgencyEngine::Director
{
    namespace
    {
        constexpr auto kPlayerFormID = static_cast<std::uint32_t>(0x14);
        // Declared in our SkyrimNet plugin manifest. It selects the LLM config
        // profile SkyrimNet uses for impulses, so the user can point impulses at a
        // cheaper model without touching their dialogue model. An unknown
        // variant just falls back to SkyrimNet's default Dialogue LLM.
        constexpr auto kLLMVariant = "agencyengine_impulse";
        // The resolution check — "has she since had this out?" — is a yes/no
        // read over an event tail, not creative writing, so it gets its own
        // variant and can be pointed at a much cheaper model than the impulse.
        constexpr auto kResolvePrompt = "agencyengine_impulse_resolved";
        constexpr auto kResolveVariant = "agencyengine_resolve";
        constexpr auto kPassInterval = 1s;
        constexpr auto kRecentEventPollInterval = 15s;
        constexpr int kRecentEventTail = 100;
        // A snapshot older than this means the main thread isn't running our
        // tasks (main menu, loading screen, hard stall) — don't act on it.
        constexpr auto kSnapshotMaxAge = 5s;
        // How long the player has to have been back — window focused, no menu,
        // frames running — before anything dispatches or is delivered. Without
        // it, an impulse held across a suspend lands within one pass of the
        // window reappearing, which reads as the mod having waited to ambush
        // them. Long enough to have your hands back on the keyboard.
        constexpr auto kResumeSettle = 10s;

        // Is the player in a position to be spoken to at all?
        //
        // Three ways of not being, and the third is the one that bites. A
        // backgrounded window stops producing frames, so our main-thread tasks
        // stop running and the snapshot goes stale — the game is suspended and
        // we cannot see it. Everything real-time in this file (the defer clock,
        // the quiet reading's age, SkyrimNet's audio and dialogue counters)
        // keeps running through that, so on the first pass back the party reads
        // as maximally quiet and a held impulse fires immediately. That is the
        // "narration the moment I alt-tab in" symptom; a stale snapshot has to
        // count as suspended for it, not merely stop us reading.
        bool IsSuspended(const GameSnapshot& snap, bool snapshotFresh)
        {
            return !snapshotFresh || !snap.valid || snap.gamePaused || !snap.windowActive;
        }

        std::thread      g_thread;
        std::atomic_bool g_running{ false };

        // Asks asked for by hand from the UI. Each entry is a lens key, or ""
        // for "whichever is nearest due" — the Status page's one button, which
        // has no lens to name. Written on the render thread, drained on the
        // Director's, so it needs its own lock.
        //
        // A deque rather than a flag because the Lenses tab draws a button per
        // row: pressing two of them is two asks, and they are independent
        // questions that must not collapse into one.
        std::mutex              g_fireLock;
        std::deque<std::string> g_fireRequests;
        // Set on a load/new game. Combat state, and our belief about who owns
        // continuous mode, do not survive one.
        std::atomic_bool g_continuousReset{ false };
        std::atomic_bool g_combatEpisodeReset{ false };
        std::atomic<std::int64_t> g_lastCaptureMs{ 0 };
        // When the game last came back from a menu or the background. 0 means
        // it has not been suspended yet this session, which is not a wait.
        std::atomic<std::int64_t> g_resumedAtMs{ 0 };
        // When the vanilla topic list last came down. 0 means it has not been
        // seen open this session, which is not a wait either.
        std::atomic<std::int64_t> g_dialogueMenuClosedAtMs{ 0 };
        SkyrimNetAPI::RecentEventRecovery g_recentEventRecovery;
        std::int64_t g_recentPollTickMs = 0;
        std::int64_t g_recentPollActiveMs = 0;
        bool g_recentPollAttemptedBaseline = false;
        bool g_recentPollBaseline = false;
        double g_recentPollLastMilliseconds = 0.0;
        std::size_t g_recentPollTailEvents = 0;
        std::size_t g_recentPollRecoveredEvents = 0;
        std::uint64_t g_recentPollFailures = 0;

        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        std::int64_t UnixNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        // Is the player back, but not back long enough to be spoken to yet?
        bool WithinResumeSettle()
        {
            const auto resumed = g_resumedAtMs.load();
            return resumed != 0 && NowMs() - resumed < std::chrono::milliseconds{ kResumeSettle }.count();
        }

        // The same question, asked by a caller that has no snapshot in hand —
        // the LLM callback's delivery hop. Reads the shared copy and judges its
        // age itself.
        bool IsSuspended()
        {
            const bool fresh =
                NowMs() - g_lastCaptureMs.load() <= std::chrono::milliseconds{ kSnapshotMaxAge }.count();
            GameSnapshot snap;
            WithState([&](Status& state) { snap = state.snapshot; });
            return IsSuspended(snap, fresh);
        }

        std::string FormatGameTime(double gameDays)
        {
            const auto day = static_cast<int>(gameDays);
            const auto frac = gameDays - day;
            const auto hour = static_cast<int>(frac * 24.0);
            const auto minute = static_cast<int>((frac * 24.0 - hour) * 60.0);
            return std::format("day {}, {:02}:{:02}", day, hour, minute);
        }

        // ---- what a dispatch carries --------------------------------------

        // Someone the model is allowed to name. Resolved to a UUID on the
        // Director thread before the call goes out, so the LLM callback — which
        // lands on a SkyrimNet worker — only ever does string matching.
        struct Participant
        {
            std::string   name;
            std::uint64_t uuid = 0;
            // Kept alongside the UUID because GenerateNPCThought is the one
            // SkyrimNet call that wants an Actor rather than a UUID, and the
            // actor can only be safely looked up back on the main thread.
            std::uint32_t formID = 0;
        };

        // What the model decided for one ask. `speak` false is the ordinary
        // answer: nobody had anything worth interrupting the day for.
        struct Decision
        {
            bool        speak = false;
            std::string speaker;    // companion name, as the model spelled it
            std::string target;     // who they turn to
            std::string narration;  // the stage direction they will speak from
            // A few words naming the subject, separate from the narration. The
            // ledger keys on this: comparing whole stage directions would never
            // match twice, and the prompt has to be able to say "already raised:
            // the coin split" in a line rather than a paragraph.
            std::string topic;
            // The stamped thought or event the model claims this rides on.
            // Observability only — logged with the delivered impulse so the log
            // can tell a cited impulse from a confabulated one. Nothing branches
            // on it, and an older prompt that doesn't return it leaves it empty.
            std::string whyNow;
        };

        // A decision with its participants resolved, on its way into the store.
        struct ImpulseDelivery
        {
            std::string   content;
            std::string   topic;
            std::string   whyNow;   // carried for the log only
            std::string   speakerName;
            std::string   targetName;
            std::uint64_t speakerUuid = 0;
            std::uint64_t targetUuid = 0;
            std::uint32_t speakerFormID = 0;
            bool          generateThought = false;
            double        gameDays = 0.0;
            std::string   lens;
            // Declared by the lens that produced it, and carried all the way to
            // the pending entry: it is what the resolution check branches on.
            bool          proposal = false;
            // That lens's ledger ring size, 0 meaning the global count.
            int           lensLedgerSlots = 0;
        };

        // ---- the cue -------------------------------------------------------
        //
        // What is left of narration. The stage direction travels by bio now, so
        // a cue carries no subject at all: it says she has something on her mind
        // and grants her the speaking turn to say it, and her bio supplies what
        // "it" is.
        //
        // ONE PER COMPANION, COALESCING. Several lenses coming due together
        // produce several carries, and narrating each would either interrupt the
        // party back to back or queue up sentences describing state minutes
        // stale. A vague sentence is the only one that stays true while carries
        // keep joining it — and being cheap enough to drop is the other half of
        // that, because dropping one costs the announcement and not the impulse.
        //
        // Written on the main thread by the carry hop, read and cleared by the
        // Director thread, so it needs its own lock — the Status mutex is held
        // only for short reads and this holds strings.
        struct PendingCue
        {
            PendingImpulses::CueOwnership ownership;
            std::uint32_t formID = 0;
            std::string speakerName;
            std::uint64_t speakerUuid = 0;
            // Real time, for maxDeferSeconds. Coalescing updates ownership but
            // deliberately never refreshes this defer clock.
            std::int64_t setAtMs = 0;
        };
        std::mutex              g_cueLock;
        std::vector<PendingCue> g_cues;

        // Manual resolution checks asked for from the UI. Written on the render
        // thread, drained on the Director thread, so it needs its own lock —
        // and a deque rather than a flag because "Check all" on a party of five
        // is five requests that must not collapse into one.
        //
        // One request names one impulse — the actor and the lens that wrote it —
        // because a companion carrying three of them has three separate
        // questions outstanding and the button is drawn per row.
        struct ResolveRequest
        {
            PendingImpulses::EntryId entryId = 0;

            bool operator==(const ResolveRequest&) const = default;
        };
        std::mutex                 g_resolveRequestLock;
        std::deque<ResolveRequest> g_resolveRequests;
        ResolutionScheduling::Scheduler g_resolutionScheduler;

        // ---- the vanilla conversation --------------------------------------
        //
        // The player talking to a quest NPC through Skyrim's own topic list is
        // invisible to every other signal in this file, and each one is blind
        // for its own reason rather than by oversight:
        //
        //   * gamePaused is UI::GameIsPaused(), which counts menus flagged
        //     kPausesGame. DialogueMenu is not one, so the simulation — and
        //     therefore game time, and therefore every lens clock — runs
        //     straight through a conversation.
        //   * recording, speechQueue and msSinceAudioEnded are SkyrimNet's
        //     microphone, SkyrimNet's TTS queue and SkyrimNet's audio clock.
        //     Vanilla dialogue is engine audio and touches none of them. The
        //     one partial exception is SkyrimNet's own voice-over of the
        //     player's chosen topic and of otherwise-silent NPCs, which does
        //     move the audio clock — which is why this misfires on some
        //     conversations and not others, rather than on all of them.
        //   * MsSinceLastDialogue() watches SkyrimNet's own dialogue event
        //     types. Vanilla dialogue emits none of them.
        //
        // So the party reads *maximally* quiet while the player is reading a
        // topic list, which is the one reading that lets a cue out. Held here
        // as a menu flag plus a settle, because DialogueMenu comes down before
        // the parting line has finished playing: the close is the start of a
        // wait, not the all-clear.

        // Watches the topic list open and close. Director thread, once a pass.
        void TrackDialogueMenu(const Settings& settings, const GameSnapshot& snap, bool snapshotFresh)
        {
            static bool previous = false;

            // A stale snapshot describes the menu as it was before the main
            // thread stopped running our tasks, so an edge read out of it is an
            // edge of unknown age — and stamping a close from one would start
            // the settle at the wrong moment entirely. Hold position; every
            // consumer below is gated on the same staleness anyway.
            if (!snapshotFresh || !snap.valid) {
                return;
            }
            if (previous == snap.dialogueMenuOpen) {
                return;
            }
            previous = snap.dialogueMenuOpen;

            if (snap.dialogueMenuOpen) {
                logger::info("Player is in a vanilla conversation — nothing is cued while the topic list is up");
            } else {
                g_dialogueMenuClosedAtMs.store(NowMs());
                logger::info("Vanilla conversation ended — {:.0f}s of quiet before anything can be cued",
                             settings.quietSeconds);
            }
        }

        // Is the player mid-conversation with somebody else, or only just out of
        // one? Not subject to any setting: see the note at the call site.
        bool InVanillaDialogue(const Settings& settings, const GameSnapshot& snap)
        {
            if (snap.dialogueMenuOpen) {
                return true;
            }
            const auto closed = g_dialogueMenuClosedAtMs.load();
            if (closed == 0) {
                return false;
            }
            // The same threshold the audio signals use, deliberately. It is
            // already the answer to "how long since anyone spoke", and a
            // conversation that has just ended is exactly that question.
            return NowMs() - closed < static_cast<std::int64_t>(settings.quietSeconds * 1000.0f);
        }

        // ---- the party's own conversation ----------------------------------
        //
        // IsQuiet below answers "is anybody speaking right now". This answers
        // "is there still a conversation going on around this gap", and they
        // are not the same question — which is the whole bug this exists for.
        //
        // quietSeconds is 25 by default, and twenty-five seconds of nobody
        // speaking is an ordinary beat in a group chat: the player composing a
        // longer line, or reading two followers' replies. Every signal IsQuiet
        // has reads that as an empty room, because every one of them is
        // instantaneous. So the cue went out mid-exchange and the companion
        // changed the subject — the same failure the vanilla-dialogue hold
        // above was written for, with a SkyrimNet conversation in place of a
        // topic list.
        //
        // The fix is a second, longer threshold on the same clock rather than a
        // bigger quietSeconds: raising that one would also delay every cue into
        // genuine silence, and would move `tail_live` and the injected gap with
        // it. This reads the same two clocks IsQuiet does and asks them for a
        // longer answer.
        //
        // Both clocks, for the reason ConversationSinceCue reads both: the
        // dialogue clock is the load-bearing one because it sees the player's
        // half, which makes no audio at all, and the audio clock catches an NPC
        // line that produced no dialogue event.
        bool InConversation(const Settings& settings, const QuietReading& reading)
        {
            const auto settleMs = static_cast<std::int64_t>(settings.conversationSettleSeconds * 1000.0f);

            // -1 is "nothing has been seen yet", not "just now" — a save with no
            // dialogue in it is not a conversation anybody is in the middle of.
            const auto sinceDialogue = SkyrimNetAPI::MsSinceLastDialogue();
            if (sinceDialogue >= 0 && sinceDialogue < settleMs) {
                return true;
            }
            // 0 is documented as "no audio has played yet", same distinction.
            return reading.valid && reading.msSinceAudioEnded > 0 && reading.msSinceAudioEnded < settleMs;
        }

        // Is the party quiet enough to speak into?
        //
        // Three signals sampled together in Papyrus, SkyrimNet's own dialogue
        // clock, and the vanilla topic list above. Every uncertain case
        // resolves to "not quiet": a late impulse costs nothing, and an early
        // one talks over the player.
        //
        // Deliberately unchanged by the settle above. This predicate also
        // answers `tail_live` for the prompt, which wants "is somebody talking
        // now", and folding the longer question into it would have the model
        // told the party is mid-conversation a minute after it stopped.
        bool IsQuiet(const Settings& settings, const GameSnapshot& snap, const QuietReading& reading)
        {
            // First, because it is the one signal that does not need the
            // Papyrus reading to be valid — and because it is true in the very
            // case the rest of them read as an empty room.
            if (InVanillaDialogue(settings, snap)) {
                return false;
            }
            // No reading yet, or the poll stopped coming back — the bridge
            // script may be missing or the VM stalled.
            if (!reading.valid) {
                return false;
            }
            if (NowMs() - reading.receivedAtMs > 3000) {
                return false;
            }
            // The player is holding the microphone. This produces no events at
            // all, so it is invisible to everything except this flag.
            if (reading.recording) {
                return false;
            }
            // Lines decided but not yet heard. This is the window a
            // timestamp-based check misses entirely, because the event log
            // looks silent while the LLM and TTS work.
            if (reading.speechQueue > 0) {
                return false;
            }
            const auto thresholdMs = static_cast<std::int64_t>(settings.quietSeconds * 1000.0f);

            // Someone took a conversational turn recently. This is the signal
            // the Papyrus ones cannot give: the audio clock only tracks NPC
            // *speech*, so the player composing a line and the several seconds
            // of LLM generation that follow it are completely silent by every
            // other measure, and the audio clock keeps climbing straight
            // through them.
            //
            // -1 means nothing has been seen yet, which is not a reason to
            // wait — a save with no dialogue in it is genuinely quiet.
            const auto sinceDialogue = SkyrimNetAPI::MsSinceLastDialogue();
            if (sinceDialogue >= 0 && sinceDialogue < thresholdMs) {
                return false;
            }

            // 0 is documented as "no audio has played yet" — after a load,
            // typically. Audio that genuinely just ended reports a small
            // positive number and is caught below.
            if (reading.msSinceAudioEnded == 0) {
                return true;
            }
            return reading.msSinceAudioEnded >= thresholdMs;
        }

        // The reading behind a hold or a release, for the log. Reconstructing
        // this from SkyrimNet's own debug output is not a thing anyone should
        // have to do twice.
        std::string DescribeReading(const GameSnapshot& snap, const QuietReading& reading)
        {
            // Reported whatever the Papyrus reading is worth, and ahead of it:
            // it is the one signal here that is still meaningful when the
            // bridge script is missing, and the only one that explains a hold
            // the other four cannot account for.
            const auto        closed = g_dialogueMenuClosedAtMs.load();
            const std::string menu =
                snap.dialogueMenuOpen ? std::string{ "open" }
                : closed == 0         ? std::string{ "none seen" }
                                      : std::format("closed {}ms ago", NowMs() - closed);

            if (!reading.valid) {
                return std::format("no reading yet, topicList={}", menu);
            }
            const auto sinceDialogue = SkyrimNetAPI::MsSinceLastDialogue();
            return std::format("recording={} speechQueue={} audio={}ms dialogue={} topicList={}", reading.recording,
                               reading.speechQueue, reading.msSinceAudioEnded,
                               sinceDialogue < 0 ? std::string{ "none seen" } : std::format("{}ms", sinceDialogue),
                               menu);
        }

        // How long the party has been quiet, phrased for the prompt. Empty when
        // there is nothing trustworthy to say, so the template can omit the
        // line rather than assert something false.
        std::string DescribeQuietGap(const QuietReading& reading)
        {
            if (!reading.valid || reading.recording || reading.speechQueue > 0) {
                return {};
            }
            if (reading.msSinceAudioEnded == 0) {
                return {};
            }
            const auto seconds = reading.msSinceAudioEnded / 1000;
            if (seconds < 60) {
                return std::format("{} seconds", seconds);
            }
            const auto minutes = seconds / 60;
            return minutes == 1 ? std::string{ "about a minute" } : std::format("about {} minutes", minutes);
        }

        // One lens, resolved out of the settings for a pass: which question gets
        // asked, which prompt file asks it, and how long the clock runs.
        struct LensChoice
        {
            // Lens::id, or the prompt file for a hand-authored lens. What the
            // clock in Status is keyed on.
            std::string key;
            std::string name;    // as typed in the UI; may be blank
            std::string prompt;  // never empty — a lens without one is not usable
            // Declared properties that decide what happens to an impulse after
            // the response. Carried from the settings row rather than matched
            // on a display name later.
            bool        proposal = false;
            int         ledgerSlots = 0;
            bool        playerTargetOnly = false;
            // This lens's own cadence. A quiet ask costs the interval; an ask
            // that carries costs interval + cooldown.
            float       intervalGameMinutes = 120.0f;
            float       cooldownGameMinutes = 0.0f;
        };

        double GameMinutesToDays(float minutes)
        {
            return static_cast<double>(minutes) / (24.0 * 60.0);
        }

        std::string LensKey(const Lens& lens)
        {
            return lens.id[0] != '\0' ? std::string{ lens.id } : std::string{ lens.prompt };
        }

        // Every lens this pass is allowed to ask. A lens needs a prompt file and
        // its enable switch; with none of them usable there is nothing to ask at
        // all, and the pass holds rather than falling back to a general prompt.
        std::vector<LensChoice> UsableLenses(const Settings& settings)
        {
            std::vector<LensChoice> out;
            for (const auto& lens : settings.lenses) {
                if (!lens.enabled || lens.prompt[0] == '\0') {
                    continue;
                }
                out.push_back(LensChoice{ LensKey(lens), lens.name, lens.prompt, lens.proposal, lens.ledgerSlots,
                                          lens.playerTargetOnly, std::max(lens.intervalGameMinutes, 1.0f),
                                          std::max(lens.cooldownGameMinutes, 0.0f) });
            }
            return out;
        }

        // ---- main thread -------------------------------------------------

        // Reads everything we need out of the game. Posted as an SKSE task, so
        // this is the only place in the Director that touches game objects.
        void CaptureSnapshot()
        {
            GameSnapshot snap;

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* calendar = RE::Calendar::GetSingleton();
            if (player && calendar) {
                snap.valid = true;
                snap.gameDays = static_cast<double>(calendar->GetCurrentGameTime());
                // Read every pass rather than once at load: it is the game's
                // setting, not ours, and any number of other mods move it
                // mid-session. Taken raw — 0 means a mod has frozen time, which
                // stops every in-game clock in here dead, and the settings page
                // says so rather than dividing by it.
                snap.timescale = calendar->GetTimescale();

                if (const char* name = player->GetDisplayFullName()) {
                    snap.playerName = name;
                }
                // Location first, cell second, worldspace last. Outdoors the
                // parent cell is an unnamed grid square, so cell-first fell all
                // the way through to the worldspace and told the model "Skyrim"
                // for every point on the map — true, and worth nothing to it.
                // The current location is "Volskygge".
                if (auto* location = player->GetCurrentLocation()) {
                    if (const char* name = location->GetFullName()) {
                        snap.location = name;
                    }
                }
                if (snap.location.empty()) {
                    if (auto* cell = player->GetParentCell()) {
                        if (const char* name = cell->GetFullName()) {
                            snap.location = name;
                        }
                    }
                }
                if (snap.location.empty()) {
                    if (auto* worldspace = player->GetWorldspace()) {
                        if (const char* name = worldspace->GetFullName()) {
                            snap.location = name;
                        }
                    }
                }
                snap.playerInCombat = player->IsInCombat();

                if (auto* ui = RE::UI::GetSingleton()) {
                    snap.gamePaused = ui->GameIsPaused();
                    // GameIsPaused() will not answer this either: it counts
                    // *pausing* menus, and DialogueMenu is not one — its flags
                    // are kUpdateUsesCursor | kDontHideCursorWhenTopmost, so
                    // the simulation runs through an entire conversation.
                    //
                    // Polled here rather than driven by a MenuOpenCloseEvent
                    // sink on purpose. Riding the snapshot means this flag
                    // inherits the staleness handling that everything else
                    // here has: if the main thread stops running our tasks,
                    // the flag goes stale and snapshotFresh goes false
                    // together, so the two can never disagree. An independent
                    // sink would go on asserting "the menu is open" across a
                    // stall we cannot see the end of. The cost is up to one
                    // pass of latency on the open edge, against a settle
                    // measured in tens of seconds.
                    snap.dialogueMenuOpen = ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
                }
                // Main::gameActive is the engine's own WM_ACTIVATE flag, which
                // is why this needs no Win32. GameIsPaused() will not answer
                // this: it counts *menus*, and alt-tabbing opens none.
                if (auto* main = RE::Main::GetSingleton()) {
                    snap.windowActive = main->gameActive;
                }

                // Followers = loaded, living actors flagged as player teammates.
                // That covers vanilla followers and every follower framework
                // that goes through SetPlayerTeammate, which is all of them.
                if (auto* processLists = RE::ProcessLists::GetSingleton()) {
                    for (auto& handle : processLists->highActorHandles) {
                        auto actor = handle.get();
                        if (!actor || actor->IsDead() || !actor->IsPlayerTeammate()) {
                            continue;
                        }
                        FollowerInfo info;
                        info.formID = actor->GetFormID();
                        if (const char* name = actor->GetDisplayFullName()) {
                            info.name = name;
                        }
                        snap.followers.push_back(std::move(info));
                    }
                }
            }

            g_lastCaptureMs.store(NowMs());
            WithState([&](Status& state) { state.snapshot = std::move(snap); });
        }

        // Touches only shared state, so unlike the rest of this section it is
        // called from the SkyrimNet worker too — a quiet ask never reaches
        // the main thread at all.
        void RecordImpulse(Impulse impulse, bool countAsSpoken)
        {
            WithState([&](Status& state) {
                if (!impulse.ok) {
                    state.lastError = "Papyrus dispatch into SkyrimNetApi failed";
                }

                if (!impulse.lens.empty()) {
                    auto tally = std::ranges::find_if(state.lensTallies, [&](const LensTally& candidate) {
                        return candidate.name == impulse.lens;
                    });
                    if (tally == state.lensTallies.end()) {
                        state.lensTallies.push_back(LensTally{ impulse.lens, 0, 0 });
                        tally = std::prev(state.lensTallies.end());
                    }
                    (countAsSpoken ? tally->carried : tally->quiet) += 1;
                }

                state.history.push_front(std::move(impulse));
                while (state.history.size() > StateStore::kHistoryCap) {
                    state.history.pop_back();
                }
                if (countAsSpoken) {
                    state.impulsesThisSession += 1;
                } else {
                    state.silencesThisSession += 1;
                }
            });
        }

        // The impulse becomes something the companion is privately chewing on.
        //
        // This asks SkyrimNet to *generate* the thought from a hint rather than
        // storing our text, and that indirection is the whole point. Writing an
        // event ourselves — any type, including npc_thoughts — gets it stamped
        // with a proximity-and-line-of-sight audience at creation, and bystanders
        // read it; measured twice, once as agencyengine_event and once as
        // npc_thoughts. Thoughts SkyrimNet creates through its own path stay with
        // their thinker: in the same prompt where our written one leaked to a
        // bystanding follower, eight of the speaker's genuine thoughts did not
        // appear at all. So we hand over a hint and let SkyrimNet own the write.
        //
        // The cost is a second LLM call and our composed line being paraphrased
        // rather than stored verbatim. Privacy is worth more than the phrasing:
        // the text is a stage direction, not dialogue, and nobody reads it but her.
        bool RecordAsThought(const ImpulseDelivery& d)
        {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(d.speakerFormID);
            if (!actor) {
                logger::warn("Wanted a private thought from {} about what she is carrying, but she is no longer "
                             "loaded ({:08X})",
                             d.speakerName, d.speakerFormID);
                return false;
            }

            // Framed as unsaid on purpose, and it stays true whether or not a cue
            // follows: a cue grants her the turn, it does not put words in her
            // mouth, and she may still be carrying this when the scene moves on.
            const auto hint = std::format(
                "Something about {} is sitting with you that you have NOT said out loud, and are not saying now: {} "
                "What are you actually thinking about it, privately?",
                d.targetName, d.content);

            return PapyrusBridge::GenerateNPCThought(actor, hint);
        }

        // The carry: the impulse is held in the DLL and reaches her next prompt
        // through the decorator SkyrimNet calls while rendering her character
        // bio — verbatim, privately, with no LLM call in between.
        //
        // This is delivery now. Narration used to carry the stage direction and
        // grant the speaking turn in one act; the two came apart when a pass
        // started being able to produce several carries at once (docs/adr/0004),
        // and this is the half that carries the payload.
        PendingImpulses::EntryId RecordAsPendingImpulse(const ImpulseDelivery& d, int ledgerSlots,
                                                        bool ledgerEnabled)
        {
            if (d.speakerFormID == 0) {
                logger::warn("Wanted to record {}'s impulse for her bio, but there is no FormID to key it to — the "
                             "impulse is lost",
                             d.speakerName);
                return 0;
            }

            PendingImpulses::Entry entry;
            entry.formID = d.speakerFormID;
            entry.speakerId = d.speakerUuid;
            entry.speakerName = d.speakerName;
            entry.target = { d.targetUuid, d.targetName };
            entry.text = d.content;
            entry.topic = d.topic;
            entry.lens = d.lens;
            entry.proposal = d.proposal;
            entry.createdGameDays = d.gameDays;
            const auto slots = d.lensLedgerSlots > 0 ? d.lensLedgerSlots : ledgerSlots;
            return PendingImpulses::Carry(std::move(entry),
                                          ledgerEnabled && slots > 0
                                              ? static_cast<std::size_t>(std::max(d.lensLedgerSlots, 0))
                                              : 0);
        }

        // The subject takes a provisional ledger slot the moment it is carried.
        //
        // At carry rather than at her saying it, because we no longer learn when
        // she says it: a cue grants a turn and SkyrimNet writes the line, so
        // which of the things she is carrying she actually raised is not
        // something this mod can observe. Carry is the event it owns.
        //
        // The slot stays provisional until the entry that owns it dies —
        // confirmed if the resolution check judges the subject met, withdrawn
        // otherwise — so nothing here settles a subject. It only stops the next
        // ask proposing the same one straight back.
        // Personal provisional memory is created atomically by Carry().

        // Sets or coalesces this companion's pending cue. Main thread, called
        // straight after the store write.
        void SetOrCoalesceCue(const ImpulseDelivery& d, PendingImpulses::EntryId entryId)
        {
            {
                std::scoped_lock lock{ g_cueLock };
                const auto it = std::ranges::find_if(
                    g_cues, [&](const PendingCue& cue) { return cue.formID == d.speakerFormID; });
                if (it != g_cues.end()) {
                    it->ownership.Coalesce(entryId, { d.targetUuid, d.targetName });
                    // The newest carry decides ownership before emission; the
                    // original defer clock remains fixed.
                    logger::info("{} already has a cue waiting — that is {} things on her mind behind one sentence",
                                 d.speakerName, it->ownership.carries);
                    return;
                }

                PendingCue cue;
                cue.ownership.Coalesce(entryId, { d.targetUuid, d.targetName });
                cue.formID = d.speakerFormID;
                cue.speakerName = d.speakerName;
                cue.speakerUuid = d.speakerUuid;
                cue.setAtMs = NowMs();
                g_cues.push_back(std::move(cue));
            }
            WithState([](Status& state) { state.deliveryPending = true; });
        }

        // Carries the finished impulse, then announces it. Main thread — the
        // Papyrus VM is not thread-safe, and the thought path needs a live actor.
        //
        // The order is the design. The store write lands first, so that by the
        // time any narration goes out her bio already says what she is carrying;
        // the other way round grants her a speaking turn before there is an
        // agenda to speak from, and she fills it with nothing.
        void CarryImpulse(ImpulseDelivery d)
        {
            const auto settings = SnapshotSettings();

            bool        ok = false;
            std::string how;
            PendingImpulses::EntryId entryId = 0;

            if (settings.pendingBioInjection) {
                entryId = RecordAsPendingImpulse(d, settings.ledgerSlots, settings.ledgerEnabled);
                ok = entryId != 0;
                how = "carried in her bio";
                if (ok && settings.generateThought) {
                    // Two different things about one impulse: the pending entry
                    // is the agenda, in her words-to-be; this is what carrying it
                    // feels like.
                    how += " + thought";
                    RecordAsThought(d);
                }
            } else {
                // The A/B against carrying: SkyrimNet generates a thought from
                // the impulse instead of the DLL holding the text. Nothing is in
                // her bio afterwards, so there is nothing for a cue to be about
                // and none is set.
                how = "generated thought";
                ok = RecordAsThought(d);
            }

            // The cue, once the impulse is genuinely in her bio and there is
            // somebody for SkyrimNet to give a turn to. Without a UUID she cannot
            // be handed one, which costs the announcement and nothing else — the
            // impulse is carried either way, and drift is the fallback rather
            // than a degrade path.
            if (ok && settings.pendingBioInjection) {
                if (!settings.cues) {
                    logger::debug("Cues are off — {} carries it, and it surfaces as it colours what she says",
                                  d.speakerName);
                } else if (d.speakerUuid == 0) {
                    logger::warn("SkyrimNet has no UUID for {}, so she cannot be given a speaking turn — the impulse "
                                 "stays in her bio and surfaces from there",
                                 d.speakerName);
                } else {
                    SetOrCoalesceCue(d, entryId);
                    how += " + cue";
                }
            }

            // Logged after the cue rather than before it, so the one line names
            // everything that happened to this impulse — including whether an
            // announcement is now waiting on a lull.
            if (ok) {
                logger::info("{} picks something up to raise with {} — {} at game time {}: {}", d.speakerName,
                             d.targetName, how, FormatGameTime(d.gameDays), d.content);
                // The impulse's own citation, straight from the model. An empty
                // one is not an error — an older prompt file simply doesn't ask
                // for it — but it is worth a line, because a prompt that should
                // be returning citations and isn't is being skimmed.
                if (d.whyNow.empty()) {
                    logger::info("No why_now came back with it — the model did not cite what this rides on");
                } else {
                    logger::info("Why now: {}", OneLine(d.whyNow));
                }
            } else {
                logger::error("Impulse FAILED to reach SkyrimNet (attempted as a {}, {} -> {}) at game time {}. The "
                              "text was: {}",
                              how, d.speakerName, d.targetName, FormatGameTime(d.gameDays), d.content);
            }

            Impulse impulse;
            impulse.when = FormatGameTime(d.gameDays);
            impulse.content = std::move(d.content);
            impulse.topic = std::move(d.topic);
            impulse.speaker = std::move(d.speakerName);
            impulse.target = std::move(d.targetName);
            impulse.delivery = std::move(how);
            impulse.lens = std::move(d.lens);
            impulse.ok = ok;
            RecordImpulse(std::move(impulse), true);
        }

        // ---- director thread ---------------------------------------------

        // SkyrimNet returns a JSON array as a string; hand it to the prompt as
        // real JSON when it parses, and as a raw string when it doesn't, so a
        // format change upstream degrades instead of breaking.
        nlohmann::json ParseEvents(const std::string& raw)
        {
            if (raw.empty()) {
                return nlohmann::json::array();
            }
            try {
                return nlohmann::json::parse(raw);
            } catch (const std::exception&) {
                return raw;
            }
        }

        void Trim(std::string& s)
        {
            const auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                s.clear();
                return;
            }
            const auto last = s.find_last_not_of(" \t\r\n");
            s = s.substr(first, last - first + 1);
        }

        // Models like wrapping their answer in a ```fence```, sometimes with a
        // language tag. Strip it.
        std::string Unfence(const std::string& response)
        {
            std::string text = response;
            Trim(text);

            if (text.starts_with("```")) {
                const auto firstNewline = text.find('\n');
                if (firstNewline != std::string::npos) {
                    text = text.substr(firstNewline + 1);
                }
                const auto closing = text.rfind("```");
                if (closing != std::string::npos) {
                    text = text.substr(0, closing);
                }
                Trim(text);
            }
            return text;
        }

        std::string FirstString(const nlohmann::json& obj, std::initializer_list<const char*> keys)
        {
            for (const auto* key : keys) {
                if (obj.contains(key) && obj[key].is_string()) {
                    auto value = obj[key].get<std::string>();
                    Trim(value);
                    return value;
                }
            }
            return {};
        }

        // The prompt asks for {"speaker", "target", "topic", "why_now",
        // "narration"}, or
        // {"speaker": null} for the ordinary case where nobody has anything to
        // raise. Everything here is written so that a model which drifts off
        // that format still produces *something* rather than costing the call.
        Decision ParseDecision(const std::string& response)
        {
            Decision decision;

            const auto text = Unfence(response);
            if (text.empty()) {
                return decision;
            }

            // Parse from the first brace to the last rather than the whole
            // string, so a sentence of preamble doesn't sink an otherwise good
            // answer.
            const auto open = text.find('{');
            const auto close = text.rfind('}');
            if (open != std::string::npos && close != std::string::npos && close > open) {
                try {
                    const auto parsed = nlohmann::json::parse(text.substr(open, close - open + 1));
                    if (parsed.is_object()) {
                        decision.narration = FirstString(parsed, { "narration", "impulse", "content", "text" });
                        decision.speaker = FirstString(parsed, { "speaker", "companion", "follower" });
                        decision.target = FirstString(parsed, { "target", "audience", "listener" });
                        decision.topic = FirstString(parsed, { "topic", "subject", "about" });
                        decision.whyNow = FirstString(parsed, { "why_now", "whyNow", "why", "evidence" });
                        // A null/absent speaker is how silence is spelled, but
                        // the narration is what we'd actually deliver — so an
                        // empty one means silence regardless of what else came
                        // back.
                        decision.speak = !decision.narration.empty();
                        return decision;
                    }
                } catch (const std::exception&) {
                    // Not JSON after all — fall through to the prose path.
                }
            }

            // No JSON at all. Either the model wrote the stage direction as
            // bare prose (usable — we just don't know who speaks) or it wrote
            // its silence in words, which must not be narrated at the party as
            // though a companion had said "None".
            std::string lowered = text;
            std::ranges::transform(lowered, lowered.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            while (!lowered.empty() && (lowered.back() == '.' || lowered.back() == '"' || lowered.back() == '\'')) {
                lowered.pop_back();
            }
            for (const auto* word : { "null", "none", "silence", "no", "nobody", "n/a", "{}" }) {
                if (lowered == word) {
                    return decision;
                }
            }

            decision.narration = text;
            decision.speak = true;
            return decision;
        }

        bool IEquals(std::string_view a, std::string_view b)
        {
            return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
                return std::tolower(x) == std::tolower(y);
            });
        }

        std::string_view FirstToken(std::string_view name)
        {
            const auto space = name.find(' ');
            return space == std::string_view::npos ? name : name.substr(0, space);
        }

        // The model answers with names, because names are all the prompt shows
        // it. Map back onto the party we actually captured — exact match first,
        // then first-name only, so "Serana" finds "Serana Volkihar" and a
        // renamed follower doesn't silently lose their voice.
        const Participant* FindParticipant(const std::vector<Participant>& roster, std::string_view name)
        {
            if (name.empty()) {
                return nullptr;
            }
            for (const auto& participant : roster) {
                if (IEquals(participant.name, name)) {
                    return &participant;
                }
            }
            for (const auto& participant : roster) {
                if (IEquals(FirstToken(participant.name), FirstToken(name))) {
                    return &participant;
                }
            }
            return nullptr;
        }

        std::string BuildContextJson(const Settings& settings, const GameSnapshot& snap, bool manual)
        {
            nlohmann::json context;
            context["player_name"] = snap.playerName;
            context["location"] = snap.location;
            context["game_time"] = FormatGameTime(snap.gameDays);
            context["in_combat"] = snap.playerInCombat;

            nlohmann::json followers = nlohmann::json::array();
            for (const auto& follower : snap.followers) {
                nlohmann::json entry;
                entry["name"] = follower.name;
                entry["formid"] = std::format("{:08X}", follower.formID);

                // Spelled UUID, and emitted as a number rather than a string,
                // to match how SkyrimNet exposes its own actors to templates
                // (player.UUID, npc.UUID). That's what makes the decorators
                // callable per-follower from the prompt —
                // get_relevant_memories(follower.UUID), decnpc(follower.UUID).
                //
                // 0 means SkyrimNet doesn't know this actor (never registered,
                // or its memory system hasn't ingested them yet). The key is
                // always present so a template can guard with
                // `{% if follower.UUID %}` — a missing key would be a render
                // error, and a render error costs the whole impulse.
                const auto uuid = SkyrimNetAPI::FormIDToUUID(follower.formID);
                entry["UUID"] = uuid;
                if (uuid == 0) {
                    logger::warn("SkyrimNet has no UUID for follower {} ({:08X}) — decorator calls for them will "
                                 "not resolve in the prompt",
                                 follower.name, follower.formID);
                }

                if (settings.perFollowerEvents > 0) {
                    // Its own filter, falling back to the shared one. The two
                    // tails want different things: the player's wants everything
                    // that happened, while this one is filtered down to thoughts
                    // by the prompt regardless — so pointing it at the thought
                    // type makes the count mean what it looks like it means.
                    const std::string followerFilter = settings.followerEventTypeFilter[0] != '\0'
                                                           ? settings.followerEventTypeFilter
                                                           : settings.eventTypeFilter;
                    entry["recent_events"] = ParseEvents(
                        SkyrimNetAPI::GetRecentEvents(follower.formID, settings.perFollowerEvents, followerFilter));
                }

                // What she has already raised. Always set, even when empty — an
                // unset name is a render error in Inja, and a render error costs
                // the whole impulse as a blank prompt.
                nlohmann::json alreadyRaised = nlohmann::json::array();
                if (settings.ledgerEnabled) {
                    for (auto& topic : PendingImpulses::LedgerTopics(follower.formID)) {
                        alreadyRaised.push_back(std::move(topic));
                    }
                }
                entry["already_raised"] = std::move(alreadyRaised);

                followers.push_back(std::move(entry));
            }
            context["followers"] = std::move(followers);

            nlohmann::json partyTopics = nlohmann::json::array();
            if (settings.ledgerEnabled) {
                for (const auto& record : PendingImpulses::PartyPromptSnapshot()) {
                    partyTopics.push_back({
                        { "entry_id", record.originEntryId },
                        { "speaker", record.speakerName },
                        { "target", record.target.name.empty() ? "the party" : record.target.name },
                        { "topic", record.topic },
                    });
                }
            }
            context["party_recent_topics"] = std::move(partyTopics);

            context["recent_events"] =
                ParseEvents(SkyrimNetAPI::GetRecentEvents(kPlayerFormID, settings.maxEvents, settings.eventTypeFilter));

            // How long since anyone spoke. This is what lets the prompt tell
            // "they stopped talking a moment ago" from "nobody has said
            // anything in an hour" — which decides whether the last exchange in
            // the event tail is still live and therefore off-limits as a
            // subject. Omitted rather than guessed when there is no trustworthy
            // reading, so the template can leave the line out entirely.
            QuietReading reading;
            WithState([&](Status& state) { reading = state.quiet; });

            // Whether the last exchange is still going, by the same predicate
            // that defers delivery. The template gates its forced-turn roll on
            // this: a forced turn mid-exchange has the silence option removed
            // and every live subject closed to it by the reacting rule, so the
            // only move left is the biggest buried thing in the state, swung at
            // full force. Always set — an unset name is a render error in Inja,
            // and that costs the whole impulse as a blank prompt.
            //
            // A manual trigger is always treated as an open tail. The UI button
            // exists to bypass gating, and someone pressing it mid-conversation
            // means it, so reporting a live tail there would quietly take the
            // forced roll away in exactly the case it was asked for.
            context["tail_live"] = !manual && !IsQuiet(settings, snap, reading);

            // The forced-turn threshold, as a percent the template rolls
            // `random` against. The roll itself stays in the prompt because
            // that is where the other half of the condition lives — a forced
            // turn is suppressed while `tail_live`, and splitting the two
            // halves across the DLL and the template is how they drift. Always
            // set, for the same reason as `tail_live`: an unset name is a
            // render error in Inja, and that costs the whole impulse.
            context["forced_impulse_chance"] = settings.forcedImpulseChance;

            if (settings.injectQuietGap) {
                if (const auto gap = DescribeQuietGap(reading); !gap.empty()) {
                    context["quiet_for"] = gap;
                }
            }

            return context.dump();
        }

        // ---- the clocks ---------------------------------------------------
        //
        // There is no draw and no shared tick. Each lens holds a game-time
        // deadline, an ask stamps the next one, and a pass on which nothing is
        // due makes no LLM calls at all. Two consecutive asks cannot repeat a
        // question for free: a lens is structurally unable to re-ask inside its
        // own interval, because it is not asked.
        //
        // Called from the Director thread; the clocks themselves live in Status
        // because the LLM callback extends them from a SkyrimNet worker.

        // Brings the clock table in line with the configured lenses and returns
        // the ones that may be asked now. Arms anything new — a fresh install, a
        // lens switched on, the first pass after a load — at now + its interval,
        // so nothing fires the instant a save comes up.
        //
        // Called on every pass that gets past the gates, including a manual one,
        // which wants the arming and the pruning even though it picks its own
        // lens afterwards.
        std::vector<LensChoice> SyncLensClocks(const std::vector<LensChoice>& usable, double nowGameDays)
        {
            std::vector<LensChoice> due;

            WithState([&](Status& state) {
                // Drop clocks for lenses that are no longer asked, so a switched-
                // off row doesn't sit on the Lenses tab counting down to nothing.
                std::erase_if(state.lensClocks, [&](const LensClock& clock) {
                    return std::ranges::none_of(usable,
                                                [&](const LensChoice& lens) { return lens.key == clock.key; });
                });

                for (const auto& lens : usable) {
                    auto it = std::ranges::find_if(state.lensClocks,
                                                   [&](const LensClock& clock) { return clock.key == lens.key; });
                    if (it == state.lensClocks.end()) {
                        state.lensClocks.push_back(LensClock{
                            lens.key, lens.name, nowGameDays + GameMinutesToDays(lens.intervalGameMinutes),
                            nowGameDays, false, false, false });
                        logger::info("The {} lens is armed at game time {} — first ask in {:.0f} in-game minutes",
                                     lens.name.empty() ? "unnamed" : lens.name, FormatGameTime(nowGameDays),
                                     lens.intervalGameMinutes);
                        continue;
                    }

                    // Game time runs backwards when an older save is loaded.
                    // Treat it as a fresh arming rather than as a deadline
                    // already met, which would ask every lens at once.
                    if (nowGameDays < it->armedGameDays) {
                        logger::info("Game time moved backwards ({} -> {}) — an older save was loaded; the {} "
                                     "lens's clock restarts",
                                     FormatGameTime(it->armedGameDays), FormatGameTime(nowGameDays), it->name);
                        it->dueGameDays = nowGameDays + GameMinutesToDays(lens.intervalGameMinutes);
                        it->armedGameDays = nowGameDays;
                        continue;
                    }

                    // The name is a label and can be edited under a stable id.
                    it->name = lens.name;

                    // Shortening a lens's cadence on the settings page has to
                    // move the countdown that is already running, or the page
                    // says "changes apply immediately" while the Next ask column
                    // sits there for another six in-game hours. Only ever
                    // *shortened*: lengthening one lets the current countdown
                    // finish and applies from the next ask, which is the
                    // conservative direction — nobody is surprised by an ask
                    // arriving on the old schedule.
                    const auto deadline = it->armedGameDays +
                                          GameMinutesToDays(lens.intervalGameMinutes +
                                                            (it->carried ? lens.cooldownGameMinutes : 0.0f));
                    it->dueGameDays = std::min(it->dueGameDays, deadline);

                    if (it->inFlight || nowGameDays < it->dueGameDays) {
                        continue;
                    }
                    due.push_back(lens);
                }
            });

            return due;
        }

        // The lens the manual trigger asks: whichever is nearest due, since the
        // button means "ask something now" and there is no longer a draw to
        // stand in for a choice. Never one already in flight.
        std::optional<LensChoice> NextLensToAsk(const std::vector<LensChoice>& usable)
        {
            std::optional<LensChoice> best;
            double                    bestDue = 0.0;
            WithState([&](Status& state) {
                for (const auto& clock : state.lensClocks) {
                    if (clock.inFlight) {
                        continue;
                    }
                    const auto it = std::ranges::find_if(usable,
                                                         [&](const LensChoice& lens) { return lens.key == clock.key; });
                    if (it == usable.end()) {
                        continue;
                    }
                    if (!best || clock.dueGameDays < bestDue) {
                        best = *it;
                        bestDue = clock.dueGameDays;
                    }
                }
            });
            return best;
        }

        // Is this lens already waiting on an answer? Only the manual path asks:
        // the scheduled one gets its answer from SyncLensClocks, which skips an
        // in-flight lens on its way to deciding what is due.
        bool AskInFlight(const std::string& key)
        {
            bool inFlight = false;
            WithState([&](Status& state) {
                for (const auto& clock : state.lensClocks) {
                    if (clock.key == key) {
                        inFlight = clock.inFlight;
                    }
                }
            });
            return inFlight;
        }

        // Why nothing was asked this pass, phrased as the next thing that will
        // happen. Named per lens because "counting down" without saying which
        // question is counting is exactly the log line that answers nothing.
        std::string DescribeNextAsk(double nowGameDays)
        {
            std::string name;
            double      soonest = 0.0;
            bool        anyInFlight = false;
            bool        found = false;
            WithState([&](Status& state) {
                for (const auto& clock : state.lensClocks) {
                    if (clock.inFlight) {
                        anyInFlight = true;
                        continue;
                    }
                    if (!found || clock.dueGameDays < soonest) {
                        soonest = clock.dueGameDays;
                        name = clock.name;
                        found = true;
                    }
                }
            });

            if (!found) {
                return anyInFlight ? std::string{ "every lens is mid-ask" } : std::string{ "no lens is armed yet" };
            }
            const auto minutes = (soonest - nowGameDays) * 24.0 * 60.0;
            return std::format("counting down — the {} lens asks in {:.0f} in-game minutes{}",
                               name.empty() ? "unnamed" : name, minutes < 0.0 ? 0.0 : minutes,
                               anyInFlight ? ", and another is mid-ask" : "");
        }

        // Stamped at dispatch, so a call that takes ten seconds does not let the
        // same lens ask again in the meantime, and a call that never comes back
        // costs one interval rather than wedging the lens for the session.
        void StampAsk(const LensChoice& lens, double nowGameDays)
        {
            WithState([&](Status& state) {
                for (auto& clock : state.lensClocks) {
                    if (clock.key != lens.key) {
                        continue;
                    }
                    clock.inFlight = true;
                    clock.asked = true;
                    clock.armedGameDays = nowGameDays;
                    // Not carried until the answer says so, so the deadline is
                    // one interval and the settings page can shorten it to one
                    // interval while the call is still out.
                    clock.carried = false;
                    clock.dueGameDays = nowGameDays + GameMinutesToDays(lens.intervalGameMinutes);
                }
                state.inFlight = true;
            });
        }

        // The ask is over. `carried` pushes the clock out by the cooldown on top
        // of the interval already stamped: carry is what a lens goes quiet for,
        // whether or not she has voiced it yet. Nothing keys on her speaking —
        // speech can lag a carry indefinitely, and a lens gated on it could
        // re-ask about a subject she is already carrying.
        //
        // Runs on a SkyrimNet worker thread as well as the Director's, hence
        // WithState rather than a plain member.
        void ReleaseAsk(const std::string& key, double askedAtGameDays, float intervalGameMinutes,
                        float cooldownGameMinutes, bool carried)
        {
            WithState([&](Status& state) {
                for (auto& clock : state.lensClocks) {
                    if (clock.key != key) {
                        continue;
                    }
                    clock.inFlight = false;
                    clock.carried = carried;
                    if (carried) {
                        clock.dueGameDays =
                            askedAtGameDays + GameMinutesToDays(intervalGameMinutes + cooldownGameMinutes);
                    }
                }
                state.inFlight = std::ranges::any_of(state.lensClocks,
                                                     [](const LensClock& clock) { return clock.inFlight; });
            });
        }

        void Fire(const Settings& settings, const GameSnapshot& snap, bool manual, const LensChoice& lens)
        {
            const auto contextJson = BuildContextJson(settings, snap, manual);

            // Resolve everyone the model is allowed to name, here on the
            // Director thread, so the callback can match names without another
            // trip into SkyrimNet.
            Participant player{ snap.playerName, SkyrimNetAPI::FormIDToUUID(kPlayerFormID), kPlayerFormID };
            std::vector<Participant> roster;
            roster.reserve(snap.followers.size());
            for (const auto& follower : snap.followers) {
                roster.push_back({ follower.name, SkyrimNetAPI::FormIDToUUID(follower.formID), follower.formID });
            }

            const auto gameDays = snap.gameDays;

            StampAsk(lens, gameDays);
            WithState([&](Status& state) {
                state.lastError.clear();
                state.lastContextJson = contextJson;
            });

            // The LLM variant stays the same across lenses on purpose: they are
            // the same job at the same cost, and one variant means one place in
            // SkyrimNet's UI to point impulses at a cheaper model.
            // The next-ask figures carry their real-time equivalent: the whole
            // question a reader has of this line is how often it happens, and
            // "in 120 in-game minutes" answers that only if they also know the
            // timescale. Guarded, because a frozen clock makes it a division by
            // zero and the answer is "never" rather than a number.
            const auto realMinutes = [scale = snap.timescale](float gameMinutes) {
                return scale > 0.0f ? std::format("{:.1f} real min", gameMinutes / scale)
                                    : std::string{ "never — the game clock is frozen" };
            };
            logger::info("Asking the {} question — prompt '{}' (variant '{}'), context {} bytes, {} follower(s). "
                         "Next {} ask in {:.0f} in-game minutes ({}), or {:.0f} ({}) if this one carries",
                         lens.name.empty() ? "unnamed" : lens.name, lens.prompt, kLLMVariant, contextJson.size(),
                         roster.size(), lens.name.empty() ? "unnamed" : lens.name, lens.intervalGameMinutes,
                         realMinutes(lens.intervalGameMinutes),
                         lens.intervalGameMinutes + lens.cooldownGameMinutes,
                         realMinutes(lens.intervalGameMinutes + lens.cooldownGameMinutes));

            if (player.uuid == 0) {
                logger::warn("SkyrimNet does not know the player's UUID — the impulse will be registered without a "
                             "target actor");
            }

            if (settings.debugLog) {
                logger::trace("Context payload: {}", OneLine(contextJson));
            } else {
                logger::debug("Context payload: {}", OneLine(Elide(contextJson)));
            }

            const auto dispatchedAt = NowMs();

            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                lens.prompt, kLLMVariant, contextJson,
                // Runs on a SkyrimNet worker thread. Nothing here touches the
                // game — the carry hop is posted to the main thread.
                [gameDays, dispatchedAt, player = std::move(player), roster = std::move(roster),
                 generateThought = settings.generateThought, lensKey = lens.key, lensName = lens.name,
                 proposal = lens.proposal, lensLedgerSlots = lens.ledgerSlots,
                 playerTargetOnly = lens.playerTargetOnly, interval = lens.intervalGameMinutes,
                 cooldown = lens.cooldownGameMinutes, ledgerVeto = settings.ledgerVeto && settings.ledgerEnabled,
                 verbose = settings.debugLog](std::string response, bool success) {
                    const auto elapsedMs = NowMs() - dispatchedAt;

                    // Every path out of this callback goes through here, and
                    // exactly one of them passes true. Called before the return
                    // rather than from a destructor so that the one place the
                    // cooldown is spent is visible in the flow.
                    const auto release = [&](bool carried) {
                        ReleaseAsk(lensKey, gameDays, interval, cooldown, carried);
                    };

                    logger::info("LLM responded after {} ms: success={}, {} bytes", elapsedMs, success,
                                 response.size());

                    if (!success) {
                        logger::error("LLM call failed after {} ms: {}", elapsedMs,
                                      response.empty() ? "(no error text supplied by SkyrimNet)" : response);
                        WithState([&](Status& state) {
                            state.lastError = response.empty() ? "LLM call failed" : response;
                        });
                        // A failed call is not a quiet ask, but it costs the same
                        // interval: retrying it next pass would turn an outage
                        // into a call every second.
                        release(false);
                        return;
                    }

                    if (verbose) {
                        logger::trace("Raw response: {}", OneLine(response));
                    } else {
                        logger::debug("Raw response: {}", OneLine(Elide(response)));
                    }

                    auto decision = ParseDecision(response);

                    // Silence is the expected answer most of the time, not a
                    // failure — but it still goes in the history, because
                    // "is it ever speaking?" is the first question anyone
                    // tuning this will ask.
                    if (!decision.speak) {
                        logger::info("Nobody had anything to raise this time ({} question). A quiet ask costs one "
                                     "interval: the next is in {:.0f} in-game minutes.",
                                     lensName.empty() ? "general" : lensName, interval);
                        Impulse quiet;
                        quiet.when = FormatGameTime(gameDays);
                        quiet.content = "(nobody had anything to raise)";
                        quiet.delivery = "silence";
                        quiet.lens = lensName;
                        quiet.ok = true;
                        RecordImpulse(std::move(quiet), false);
                        release(false);
                        return;
                    }

                    // Who speaks is the model's call — it picked the companion
                    // with something to say. Falling back to the first follower
                    // would put someone else's words in their mouth, so an
                    // unresolvable name only degrades to that when there is
                    // genuinely no better option.
                    const auto* speaker = FindParticipant(roster, decision.speaker);
                    if (!speaker) {
                        if (roster.empty()) {
                            logger::warn("The model wrote an impulse but no followers are present to speak it; "
                                         "dropping it. Text was: {}",
                                         OneLine(decision.narration));
                            WithState([&](Status& state) {
                                state.lastError = "impulse returned with nobody present to speak it";
                            });
                            release(false);
                            return;
                        }
                        logger::warn("The model named '{}' as the speaker, who is not in the party — falling back "
                                     "to {}",
                                     decision.speaker.empty() ? "(nobody)" : decision.speaker, roster.front().name);
                        speaker = &roster.front();
                    }

                    // Target defaults to the player. Most lenses may turn to
                    // another present companion when the model names one. A
                    // player-only lens keeps its declared target semantics even
                    // when the model returns a contract-violating name.
                    const Participant* target = &player;
                    if (playerTargetOnly) {
                        if (!decision.target.empty() && !IEquals(decision.target, player.name)) {
                            logger::warn("The {} lens named '{}' as its target, but this lens always addresses {}",
                                         lensName.empty() ? "unnamed" : lensName, decision.target, player.name);
                        }
                    } else if (!decision.target.empty() && !IEquals(decision.target, player.name)) {
                        if (const auto* named = FindParticipant(roster, decision.target);
                            named && named != speaker) {
                            target = named;
                        } else if (!named) {
                            logger::warn("The model named '{}' as the target, who is not present — addressing {} "
                                         "instead",
                                         decision.target, player.name);
                        }
                    }

                    if (speaker->uuid == 0) {
                        logger::warn("SkyrimNet has no UUID for {} — they cannot be given a speaking turn, so this "
                                     "impulse will be carried in their bio without a cue to announce it",
                                     speaker->name);
                    }

                    // Exact personal suppression spans every lens. Recent
                    // party-heard suppression also checks the selected target,
                    // so another follower cannot echo the same subject.
                    const bool personalRepeat =
                        ledgerVeto && !decision.topic.empty() &&
                        PendingImpulses::LedgerSuppresses(speaker->formID, decision.topic);
                    const bool partyRepeat =
                        ledgerVeto && !decision.topic.empty() &&
                        PendingImpulses::PartySuppresses(decision.topic, { target->uuid, target->name });
                    if (personalRepeat || partyRepeat) {
                        logger::info("Held back: {}'s '{}' is already present in {} memory. Nothing is carried from "
                                     "this ask.",
                                     speaker->name, OneLine(decision.topic),
                                     partyRepeat ? "recent party-heard" : "personal");
                        Impulse held;
                        held.when = FormatGameTime(gameDays);
                        held.content = std::format("(held back: '{}' is already remembered by {})",
                                                   OneLine(decision.topic),
                                                   partyRepeat ? "the party" : speaker->name);
                        held.speaker = speaker->name;
                        held.topic = decision.topic;
                        held.delivery = partyRepeat ? "held (party heard)" : "held (personally remembered)";
                        held.lens = lensName;
                        held.ok = true;
                        RecordImpulse(std::move(held), false);
                        // Nothing is carried, so this cost an interval and not
                        // the cooldown — the lens gets to try a different subject
                        // rather than being silenced for the model's repeat.
                        release(false);
                        return;
                    }

                    // A topic-less answer still delivers — an older prompt, or a
                    // model that dropped the field, should cost a line of log
                    // rather than the impulse. It just leaves no ledger slot, so
                    // that subject is unsuppressed until the model names one.
                    if (decision.topic.empty()) {
                        logger::warn("The model returned an impulse for {} with no topic — it will be delivered, but "
                                     "nothing will stop it being raised again",
                                     speaker->name);
                    }

                    ImpulseDelivery outgoing;
                    outgoing.content = std::move(decision.narration);
                    outgoing.topic = std::move(decision.topic);
                    outgoing.whyNow = std::move(decision.whyNow);
                    outgoing.speakerName = speaker->name;
                    outgoing.targetName = target->name;
                    outgoing.speakerUuid = speaker->uuid;
                    outgoing.targetUuid = target->uuid;
                    outgoing.speakerFormID = speaker->formID;
                    outgoing.generateThought = generateThought;
                    outgoing.gameDays = gameDays;
                    outgoing.lens = lensName;
                    outgoing.proposal = proposal;
                    outgoing.lensLedgerSlots = lensLedgerSlots;

                    // The ask produced something to carry, so this lens goes
                    // quiet for the cooldown as well as the interval. Stamped
                    // here rather than after the carry lands: the carry hops to
                    // the main thread, and a lens that stayed askable across that
                    // gap could produce a second impulse about the same day for
                    // the same companion.
                    logger::info("The {} lens carried something, so it goes quiet for {:.0f} in-game minutes",
                                 lensName.empty() ? "unnamed" : lensName, interval + cooldown);
                    release(true);

                    SKSE::GetTaskInterface()->AddTask(
                        [outgoing = std::move(outgoing)]() mutable { CarryImpulse(std::move(outgoing)); });
                });

            if (!queued) {
                ReleaseAsk(lens.key, gameDays, lens.intervalGameMinutes, lens.cooldownGameMinutes, false);
                WithState([&](Status& state) {
                    state.lastError = "SkyrimNet refused the LLM request (queue full or API unavailable)";
                });
                logger::error("SkyrimNet refused the LLM request");
            }
        }

        // ---- narration of the loop's own state ----------------------------
        //
        // Both helpers below are called only from the Director thread, so their
        // statics need no synchronisation.

        // Logs a hold reason when it *changes*. The loop passes once a second;
        // logging every pass would make the file useless, and logging nothing
        // makes "why didn't it fire" unanswerable. Transitions are the
        // information.
        void NoteHold(std::string reason)
        {
            static std::string previous{ "<not yet evaluated>" };

            if (reason != previous) {
                if (reason.empty()) {
                    logger::info("Hold cleared — all gates passed");
                } else {
                    logger::info("Holding: {}", reason);
                }
                previous = reason;
            }

            WithState([&](Status& state) { state.holdReason = reason; });
        }

        // Party and situation changes, logged as they happen. For a mod about
        // followers, "who was actually present when this impulse was written" is
        // the context that makes a bad impulse explicable after the fact.
        void LogSnapshotChanges(const GameSnapshot& snap, bool verbose)
        {
            static bool                     havePrevious = false;
            static std::vector<std::string> previousFollowers;
            static bool                     previousInCombat = false;
            static std::string              previousLocation;
            static float                    previousTimescale = 0.0f;

            if (verbose) {
                logger::trace(
                    "Snapshot: valid={} gameTime={} location='{}' inCombat={} paused={} windowActive={} "
                    "topicList={} followers={}",
                    snap.valid, FormatGameTime(snap.gameDays), snap.location, snap.playerInCombat, snap.gamePaused,
                    snap.windowActive, snap.dialogueMenuOpen, snap.followers.size());
            }

            if (!snap.valid) {
                return;
            }

            std::vector<std::string> current;
            current.reserve(snap.followers.size());
            for (const auto& follower : snap.followers) {
                current.push_back(std::format("{} ({:08X})", follower.name, follower.formID));
            }

            if (!havePrevious) {
                havePrevious = true;
                previousFollowers = current;
                previousInCombat = snap.playerInCombat;
                previousLocation = snap.location;
                previousTimescale = snap.timescale;
                // Named on the first snapshot and again whenever it moves,
                // because every cadence this log quotes is in in-game minutes
                // and this is the only line that says what one of those costs
                // in real time. A log from a timescale-6 modlist and one from a
                // vanilla install describe completely different ask rates off
                // identical settings.
                logger::info("Timescale is {:.1f} — one real minute is {:.1f} in-game minutes, so every in-game "
                             "interval below is 1/{:.1f} of itself in real time",
                             snap.timescale, snap.timescale, snap.timescale);
                logger::info("First world snapshot: game time {}, location '{}', {} follower(s){}",
                             FormatGameTime(snap.gameDays), snap.location.empty() ? "unknown" : snap.location,
                             current.size(), current.empty() ? "" : ": ");
                for (const auto& name : current) {
                    logger::info("  follower present: {}", name);
                }
                return;
            }

            for (const auto& name : current) {
                if (std::ranges::find(previousFollowers, name) == previousFollowers.end()) {
                    logger::info("Follower joined the party: {}", name);
                }
            }
            for (const auto& name : previousFollowers) {
                if (std::ranges::find(current, name) == current.end()) {
                    logger::info("Follower left the party (dismissed, dead, or unloaded): {}", name);
                }
            }
            previousFollowers = current;

            if (snap.playerInCombat != previousInCombat) {
                logger::info("Player {} combat", snap.playerInCombat ? "entered" : "left");
                previousInCombat = snap.playerInCombat;
            }

            if (snap.location != previousLocation) {
                logger::info("Location changed: '{}' -> '{}'", previousLocation.empty() ? "unknown" : previousLocation,
                             snap.location.empty() ? "unknown" : snap.location);
                previousLocation = snap.location;
            }

            // On change only, like everything else here. It moves rarely — a
            // settings page, another mod, or a script freezing time — and each
            // time it does, every cadence in this log starts meaning something
            // different in real minutes.
            if (std::abs(snap.timescale - previousTimescale) > 0.01f) {
                logger::info("Timescale changed: {:.1f} -> {:.1f} in-game minutes per real minute. Every lens "
                             "interval and cooldown now costs {} real time than it did.",
                             previousTimescale, snap.timescale,
                             snap.timescale > previousTimescale ? "less" : "more");
                if (snap.timescale <= 0.0f) {
                    logger::warn("Timescale is 0 — the game clock is frozen, so no lens will come due and "
                                 "nothing carried will expire until it moves again");
                }
                previousTimescale = snap.timescale;
            }
        }

        // ---- combat episode consumers -------------------------------------
        //
        // The tracker owns the definition of a fight. Both consumers below see
        // the same exit grace, brief-combat-drop handling, and suspension clock.
        // Neither is subject to the impulse gates.
        void QueueCombatEvent(const CombatEpisodeSignal& signal)
        {
            const auto phase = std::string{ CombatEpisodePhaseName(signal.phase) };
            const auto playerUuid = SkyrimNetAPI::FormIDToUUID(kPlayerFormID);
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([phase, sequence = signal.sequence, elapsed = signal.elapsedSeconds, playerUuid]() {
                    PapyrusBridge::RecordCombatEvent(phase, sequence, elapsed, playerUuid);
                });
            }
        }

        // The whole continuous-mode read-modify-write lives in Papyrus
        // (AgencyEngine_Bridge), so everything here is fire-and-forget;
        // ownership comes back through the mod-event sink.
        void UpdateContinuousMode(const Settings& settings, bool inEpisode, bool suspended)
        {
            // Director thread only, like the other helpers in this section.
            static bool         active = false;      // this consumer is holding this episode
            static int          acquireReports = -1; // acquire count at acquire, -1 = nothing outstanding
            static std::int64_t acquiredMs = 0;
            // Switching the mode off is a simulated hotkey press, and SkyrimNet
            // drops one that lands while Papyrus is frozen. Ownership remains
            // in Status until a report confirms that the toggle took.
            static std::int64_t releaseAskedMs = 0;
            static int          releaseAttempts = 0;
            static const char*  releaseWhy = "combat ended";

            constexpr std::int64_t kReleaseRetryMs = 5000;
            constexpr int          kReleaseAttempts = 3;

            bool owned = false;
            int  reports = 0;
            WithState([&](Status& state) {
                owned = state.continuousOwned;
                reports = state.continuousAcquireReports;
            });

            const auto release = [&](const char* why) {
                logger::info("Releasing continuous mode: {}", why);
                releaseAskedMs = NowMs();
                releaseAttempts += 1;
                WithState([](Status& state) { state.continuousPending = true; });
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() { PapyrusBridge::SetContinuousMode(false); });
                }
            };

            if (g_continuousReset.exchange(false)) {
                // A load invalidates the edge this consumer was following, but
                // SkyrimNet's runtime setting survives. Preserve ownership so
                // the next running pass either adopts the loaded fight or hands
                // the mode back.
                if (owned) {
                    releaseWhy = "a save was loaded while we were holding it";
                }
                active = false;
                acquireReports = -1;
                releaseAskedMs = 0;
                releaseAttempts = 0;
            }

            if (!settings.combatContinuousMode && active) {
                if (owned) {
                    releaseWhy = "the setting was switched off while we were holding it";
                }
                active = false;
                acquireReports = -1;
            }

            // Never send a simulated hotkey while the VM is paused or the
            // snapshot is stale. The shared episode tracker also pauses here.
            if (suspended) {
                return;
            }

            const bool inFight = settings.combatContinuousMode && inEpisode;

            // Anything we hold outside a configured fight is owed back. Keep
            // asking until a report says the mode is actually off.
            if (owned && !active && !inFight) {
                const auto waited = NowMs() - releaseAskedMs;
                if (releaseAttempts == 0) {
                    release(releaseWhy);
                } else if (waited < kReleaseRetryMs) {
                    // A report is still on its way.
                } else if (releaseAttempts < kReleaseAttempts) {
                    release("the last switch-off didn't take");
                } else if (releaseAttempts == kReleaseAttempts) {
                    logger::warn("Continuous mode is still on after {} attempts to switch it off. Leaving it for "
                                 "now — the next fight to end will try again, or press SkyrimNet's continuous-mode "
                                 "hotkey to clear it by hand.",
                                 kReleaseAttempts);
                    releaseAttempts += 1;
                }
                return;
            }

            if (inFight) {
                if (active) {
                    return;
                }
                active = true;
                acquireReports = reports;
                acquiredMs = NowMs();
                releaseAskedMs = 0;
                releaseAttempts = 0;
                releaseWhy = "combat ended";
                logger::info("Combat episode started — asking for continuous mode");
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    WithState([](Status& state) { state.continuousPending = true; });
                    tasks->AddTask([]() { PapyrusBridge::SetContinuousMode(true); });
                }
                return;
            }

            if (!active) {
                return;
            }

            // The shared tracker has completed the exit grace. A fight shorter
            // than the Papyrus round trip still waits for its acquire report,
            // otherwise \"not owned\" is indistinguishable from \"the player
            // already had it on\" and the mode can be orphaned.
            if (acquireReports >= 0 && reports == acquireReports) {
                if (NowMs() - acquiredMs < 5000) {
                    return;
                }
                logger::warn("No reply from AgencyEngine_Bridge 5s after asking for continuous mode — giving up on "
                             "this fight. Is Scripts/AgencyEngine_Bridge.pex installed?");
            }

            active = false;
            acquireReports = -1;

            if (owned) {
                releaseWhy = "combat ended";
            } else {
                logger::debug("Combat ended, but continuous mode isn't ours to switch off — leaving it as it is");
            }
        }

        void UpdateCombat(const Settings& settings,
                          const GameSnapshot& snap,
                          bool snapshotFresh,
                          bool skyrimNetAvailable)
        {
            static CombatEpisodeTracker tracker;
            static bool                 eventsWereEnabled = false;

            if (g_combatEpisodeReset.exchange(false)) {
                tracker.Reset();
                eventsWereEnabled = false;
            }

            // Enabling the stream halfway through a fight begins a fresh event
            // lifecycle immediately, rather than emitting ongoing without a
            // preceding started signal. Continuous mode already held for the
            // encounter is unaffected.
            if (settings.combatEventsEnabled && !eventsWereEnabled) {
                tracker.Reset();
            }
            eventsWereEnabled = settings.combatEventsEnabled;

            const bool suspended = IsSuspended(snap, snapshotFresh);
            const bool needed = settings.combatEventsEnabled || settings.combatContinuousMode;
            std::optional<CombatEpisodeSignal> signal;

            if (!skyrimNetAvailable || !needed) {
                tracker.Reset();
            } else {
                const auto intervalMs =
                    static_cast<std::int64_t>(settings.combatEventIntervalSeconds * 1000.0f);
                const auto graceMs =
                    static_cast<std::int64_t>(settings.continuousExitGraceSeconds * 1000.0f);
                signal = tracker.Observe(snap.playerInCombat, suspended, NowMs(), intervalMs, graceMs);
            }

            if (!skyrimNetAvailable) {
                return;
            }

            UpdateContinuousMode(settings, tracker.InEpisode(), suspended);
            if (settings.combatEventsEnabled && signal) {
                QueueCombatEvent(*signal);
            }
        }

        // Asks Papyrus for a fresh quiet reading, at most every
        // quietPollSeconds. Director thread, so the static needs no lock.
        void PumpQuietPoll(const Settings& settings)
        {
            static std::int64_t lastPollMs = 0;

            // Unconditional: the reading has a third consumer now. `tail_live`
            // goes into every prompt and gates the forced-turn roll, so a stale
            // reading here reads as "mid-exchange" and silently switches
            // forcing off for anyone who turned both of the other two off.

            const auto now = NowMs();
            if (now - lastPollMs < static_cast<std::int64_t>(settings.quietPollSeconds * 1000.0f)) {
                return;
            }
            lastPollMs = now;

            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([]() { PapyrusBridge::PollQuiet(); });
            }
        }

        // Did anybody talk between the cue being set and now?
        //
        // The two clocks are read against the cue's own age, so this asks
        // exactly the question the resolve gate needs: was there an exchange
        // inside the window, not is there one now. The dialogue clock is the
        // load-bearing one — it sees the player's half, which makes no audio at
        // all — and the audio clock catches an NPC line that produced no
        // dialogue event.
        bool ConversationSinceCue(const PendingCue& cue, const QuietReading& reading)
        {
            const auto windowMs = NowMs() - cue.setAtMs;
            const auto sinceDialogue = SkyrimNetAPI::MsSinceLastDialogue();
            if (sinceDialogue >= 0 && sinceDialogue < windowMs) {
                return true;
            }
            // 0 is documented as "no audio has played yet", not "just now".
            return reading.valid && reading.msSinceAudioEnded > 0 && reading.msSinceAudioEnded < windowMs;
        }

        // The vague sentence itself. It names the companion and nothing else:
        // her bio carries the subject, and a cue that named one would be false
        // the moment a second carry coalesced into it.
        //
        // Third person, present tense, and no quoted speech — it is a stage
        // direction for SkyrimNet to write a line from, exactly like the impulse
        // text, and the same rules apply to it.
        std::string CueText(const PendingCue& cue)
        {
            return std::format("{} has something on their mind that they have been meaning to bring up, and the "
                               "quiet has given them the opening to start it.",
                               cue.speakerName);
        }

        // Fires a companion's cue the moment the party goes quiet, or gives up
        // on it. Director thread.
        //
        // Called on every pass, including the ones where the world is not
        // readable — that is the whole point. This is the only thing in the
        // file whose clock has to keep being *stopped* while the game is
        // suspended, and it cannot do that from behind a freshness guard.
        void PumpPendingCues(const Settings& settings, const GameSnapshot& snap, bool suspended)
        {
            static std::int64_t lastPumpMs = 0;

            const auto now = NowMs();
            const auto sinceLastPump = lastPumpMs == 0 ? 0 : now - lastPumpMs;
            lastPumpMs = now;

            struct Firing
            {
                PendingCue cue;
                std::int64_t heldMs = 0;
            };
            std::vector<Firing>                               firing;
            std::vector<std::pair<std::uint32_t, std::string>> resolveFirst;
            double                                            oldestHeldSeconds = 0.0;
            std::size_t                                       stillWaiting = 0;

            {
                // Nothing waiting is the ordinary state of this function, and
                // it should cost one uncontended lock rather than a copy of
                // every carried impulse in the party. The pump clock above is
                // already stamped, so a long quiet spell cannot make the next
                // suspended cue jump.
                std::scoped_lock lock{ g_cueLock };
                if (g_cues.empty()) {
                    return;
                }
            }

            // Everything the entries decide is read once, outside the cue lock:
            // PendingImpulses has its own, and taking the two together in one
            // order here and another order somewhere else is the deadlock nobody
            // finds until it happens.
            const auto entries = PendingImpulses::Snapshot();
            QuietReading reading;
            WithState([&](Status& state) { reading = state.quiet; });
            // Outside the deferOnConversation switch, and deliberately so.
            // That setting is a preference about SkyrimNet conversations —
            // whether a cue waits for the party's own talk to finish. Cutting
            // across the player's conversation with a quest NPC is not a
            // preference, it is the mod talking over the game, and somebody who
            // switched deferral off did not ask for that.
            const bool inDialogue = InVanillaDialogue(settings, snap);
            // Inside the switch, unlike the vanilla-dialogue hold above it: a
            // conversation with the party is precisely what deferOnConversation
            // is a preference about, and somebody who switched it off asked for
            // cues that do not wait on the party's own talk.
            const bool inConversation = settings.deferOnConversation && InConversation(settings, reading);
            const bool quiet = !inDialogue && !inConversation &&
                               (!settings.deferOnConversation || IsQuiet(settings, snap, reading));
            const bool waiting = suspended || WithinResumeSettle();

            {
                std::scoped_lock lock{ g_cueLock };
                for (auto it = g_cues.begin(); it != g_cues.end();) {
                    // The defer clock is real time, and being suspended does not
                    // stop real time. Left alone, a two-minute alt-tab exhausts
                    // maxDeferSeconds against a party that was never talking, and
                    // the cue is dropped for a conversation that never happened.
                    // Push the start forward instead, so suspended time is not
                    // held time.
                    //
                    // A vanilla conversation is the same argument and is not
                    // optional: maxDeferSeconds is 60 by default and a quest
                    // conversation runs well past it, so holding delivery
                    // *without* stopping this clock would only trade talking
                    // over the player for dropping the cue a minute into it.
                    // Time the player spent talking to somebody else is not
                    // time this cue spent failing to find a gap.
                    //
                    // The party's own conversation is the same argument again,
                    // and the settle makes it sharper rather than weaker: the
                    // hold runs for conversationSettleSeconds *past* the last
                    // word, which is 100 by default against a 60-second budget.
                    // Adding the settle without stopping the clock here would
                    // not fix the interruption, it would delete the cue instead
                    // — every one set during a chat would expire before the
                    // gate it is waiting on could ever open.
                    if (suspended || inDialogue || inConversation) {
                        it->setAtMs += sinceLastPump;
                    }
                    const auto heldMs = now - it->setAtMs;
                    oldestHeldSeconds = std::max(oldestHeldSeconds, static_cast<double>(heldMs) / 1000.0);

                    // Nobody is there to hear it, or they have only just sat back
                    // down. The quiet reading is not consulted at all in that
                    // state: after a suspend it describes a room that was frozen,
                    // not one that fell silent.
                    if (waiting) {
                        ++it;
                        continue;
                    }

                    if (heldMs >= static_cast<std::int64_t>(settings.maxDeferSeconds * 1000.0f)) {
                        // Dropped, and that is the end of it. What she is
                        // carrying is already in her bio, so the subject goes on
                        // colouring what she says — losing the cue costs the
                        // opening, not the impulse.
                        logger::info("{}'s cue never found a gap in {:.0f}s — dropping it. She is still carrying "
                                     "what it was for, and it colours what she says next.",
                                     it->speakerName, heldMs / 1000.0);
                        it = g_cues.erase(it);
                        continue;
                    }

                    if (!quiet) {
                        ++it;
                        continue;
                    }

                    // The party is quiet, so the cue can go out — unless
                    // somebody talked while it was waiting, in which case what
                    // she is carrying may have been dealt with in that very
                    // exchange, and announcing it would have her raise a subject
                    // the party just closed.

                    firing.push_back(Firing{ *it, heldMs });
                    it = g_cues.erase(it);
                }
                stillWaiting = g_cues.size();
            }


            // The UI's one line about waiting: how long the oldest cue has been
            // held, and whether any is still held at all.
            WithState([&](Status& state) {
                state.deliveryPending = stillWaiting > 0;
                state.deliveryHeldSeconds = stillWaiting > 0 ? oldestHeldSeconds : 0.0;
            });

            for (auto& pending : firing) {
                // Two different numbers, and conflating them is how the log
                // lies during tuning: `carries` is how many fresh impulses
                // coalesced into this one cue, while what she is *carrying* is
                // everything still open in her bio — which includes whatever
                // survived earlier cues. The stacking question this feature has
                // open is about the second number.
                const auto open = std::ranges::count_if(entries, [&](const PendingImpulses::Entry& e) {
                    return e.formID == pending.cue.formID && !e.unverified;
                });
                logger::info("The party went quiet after {:.0f}s — cueing {}: {} fresh carr(ies) behind the cue, "
                             "{} open in her bio. Reading: {}",
                             pending.heldMs / 1000.0, pending.cue.speakerName, pending.cue.ownership.carries, open,
                             DescribeReading(snap, reading));
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([cue = pending.cue]() {
                        if (!PapyrusBridge::DirectNarration(CueText(cue), cue.speakerUuid,
                                                            cue.ownership.target.id)) {
                            logger::error("The cue for {} failed to reach SkyrimNet — she keeps carrying it, and it "
                                          "colours what she says next",
                                          cue.speakerName);
                            return;
                        }
                        // Only on success, and only here. This is what turns the
                        // carried block in her bio from "it colours what you say"
                        // into "raise it" — so a cue that never reached SkyrimNet
                        // must not license her to open a subject she was given no
                        // turn for. Stamped before SkyrimNet renders the prompt
                        // this narration produces, which is the one call it is
                        // for; the cue itself is already erased by now, which is
                        // why the grant is recorded rather than the cue read.
                        PendingImpulses::GrantFloor(cue.ownership.entryId, cue.formID, cue.ownership.target,
                                                    cue.speakerUuid);
                    });
                }
            }
        }

        // ---- the pending impulse: expiry, verification, persistence --------
        //
        // Director thread. Everything here is bookkeeping around the entry the
        // bio decorator renders; the decorator itself never calls into this file.

        // FormIDs are load-order dependent, so an entry restored from the
        // sidecar can name a different actor after the user installs or removes
        // a mod. Match each restored FormID against a live form and drop the
        // ones that no longer resolve to the companion they were written for.
        // Until this runs, PendingImpulses::Get withholds them — a
        // misattributed agenda in somebody's bio is precisely the failure this
        // design exists to prevent.
        void VerifyRestoredImpulses()
        {
            auto restored = PendingImpulses::TakeUnverified();
            if (restored.empty()) {
                return;
            }

            SKSE::GetTaskInterface()->AddTask([restored = std::move(restored)]() {
                for (const auto& entry : restored) {
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.formID);
                    // Both checks are about the *actor*, not about one of her
                    // subjects, so they take everything she was carrying with
                    // them — a FormID that now names somebody else names them
                    // for every lens at once.
                    if (!actor) {
                        PendingImpulses::StopCarryingActor(
                            entry.formID, "stale (no actor with that FormID — load order changed)");
                        continue;
                    }
                    const char* name = actor->GetDisplayFullName();
                    if (!name || entry.speakerName != name) {
                        PendingImpulses::StopCarryingActor(
                            entry.formID, std::format("stale ({:08X} is now '{}', not '{}')", entry.formID,
                                                     name ? name : "(unnamed)", entry.speakerName));
                        continue;
                    }
                    logger::info("{} is still carrying something unsaid from before the load: {}", entry.speakerName,
                                 OneLine(entry.text));
                }
            });
        }

        // Asks the LLM whether a pending impulse has since been dealt with, one
        // at a time, at its own in-game cadence.
        // Manual paid checks remain explicit. Automatic checks are dispatched
        // by the evidence scheduler, never by elapsed game time.
        void PumpResolutionCheck(const Settings& settings, const GameSnapshot& snap)
        {
            if (const auto completed = g_resolutionScheduler.TakeResult()) {
                if (!completed->success) {
                    logger::warn("Resolution batch {} failed: {}", completed->batch.token.batchId,
                                 completed->response.empty() ? "(no error text)" :
                                                               OneLine(Elide(completed->response)));
                } else {
                    const auto verdicts =
                        ResolutionScheduling::Scheduler::ParseVerdicts(completed->response, completed->batch);
                    for (const auto& verdict : verdicts) {
                        if (verdict.state == PendingImpulses::LifecycleState::RaisedUnmet) {
                            PendingImpulses::MarkRaised(verdict.id, snap.gameDays);
                        } else if (verdict.state == PendingImpulses::LifecycleState::Met) {
                            PendingImpulses::MarkMet(verdict.id, snap.gameDays);
                        }
                    }
                    WithState([&](Status& state) {
                        state.resolutionEntriesClassified += verdicts.size();
                    });
                    logger::info("Resolution batch {} applied {} of {} requested verdicts",
                                 completed->batch.token.batchId, verdicts.size(),
                                 completed->batch.entries.size());
                }
            }

            std::vector<PendingImpulses::EntryId> manual;
            {
                std::scoped_lock lock{ g_resolveRequestLock };
                while (!g_resolveRequests.empty()) {
                    manual.push_back(g_resolveRequests.front().entryId);
                    g_resolveRequests.pop_front();
                }
            }
            g_resolutionScheduler.QueueManual(manual);

            const auto entries = PendingImpulses::Snapshot();
            std::vector<PendingImpulses::EntryId> fallback;
            for (const auto& entry : entries) {
                if (entry.state != PendingImpulses::LifecycleState::RaisedUnmet || !entry.proposal ||
                    entry.fallbackConsumed || settings.pendingTtlGameMinutes <= 0.0f) {
                    continue;
                }
                const auto ageMinutes = (snap.gameDays - entry.raisedGameDays) * 24.0 * 60.0;
                const auto remaining = settings.pendingTtlGameMinutes - ageMinutes;
                if (remaining > 0.0 &&
                    remaining <= std::max(30.0, settings.pendingTtlGameMinutes * 0.1)) {
                    fallback.push_back(entry.id);
                }
            }
            g_resolutionScheduler.QueueFallback(fallback);

            const auto cooldownMs =
                static_cast<std::int64_t>(settings.pendingResolveCooldownSeconds * 1000.0f);
            const auto batch = g_resolutionScheduler.TryDispatch(
                entries, settings.pendingResolveEventInterval, cooldownMs, NowMs(),
                [] { return PendingImpulses::NextEvidenceSequence(); });
            if (!batch) {
                return;
            }

            nlohmann::json context;
            context["trigger"] = ResolutionScheduling::ToString(batch->trigger);
            context["game_time"] = FormatGameTime(snap.gameDays);
            // Callback evidence supplies stable watermarks and eligibility, but
            // a paid resolver also needs the same bounded SkyrimNet history
            // that impulse generation sees. This is context only: callback
            // sequence IDs remain the authority for automatic retriggering.
            context["recent_events"] =
                ParseEvents(SkyrimNetAPI::GetRecentEvents(kPlayerFormID, settings.maxEvents,
                                                          settings.eventTypeFilter));
            context["follower_recent_events"] = nlohmann::json::array();
            std::set<std::uint32_t> includedFollowers;
            const std::string followerFilter = settings.followerEventTypeFilter[0] != '\0'
                                                   ? settings.followerEventTypeFilter
                                                   : settings.eventTypeFilter;
            for (const auto& item : batch->entries) {
                if (!includedFollowers.insert(item.entry.formID).second) {
                    continue;
                }
                context["follower_recent_events"].push_back({
                    { "speaker", item.entry.speakerName },
                    { "formid", std::format("{:08X}", item.entry.formID) },
                    { "events", ParseEvents(SkyrimNetAPI::GetRecentEvents(
                                    item.entry.formID, settings.perFollowerEvents, followerFilter)) },
                });
            }
            context["entries"] = nlohmann::json::array();
            for (const auto& item : batch->entries) {
                context["entries"].push_back({
                    { "id", item.entry.id },
                    { "speaker", item.entry.speakerName },
                    { "target", item.entry.target.name },
                    { "topic", item.entry.topic },
                    { "impulse", item.entry.text },
                    { "kind", item.entry.proposal ? "proposal" : "topic" },
                    { "state", PendingImpulses::ToString(item.entry.state) },
                    { "marked_at", FormatGameTime(
                                       item.entry.state == PendingImpulses::LifecycleState::RaisedUnmet
                                           ? item.entry.raisedGameDays
                                           : item.entry.createdGameDays) },
                    { "relevant_event_ids", item.relevantEventIds },
                });
                PendingImpulses::SetLastAttemptedEvidenceSequence(item.entry.id, batch->upperSequence);
                if (batch->trigger == ResolutionScheduling::Trigger::PreExpiry && item.entry.proposal) {
                    PendingImpulses::MarkFallbackConsumed(item.entry.id);
                }
            }
            context["events"] = nlohmann::json::array();
            for (const auto& event : batch->events) {
                context["events"].push_back({
                    { "id", event.sequence },
                    { "source_id", event.sourceId },
                    { "type", event.type },
                    { "actor_id", event.actorId },
                    { "target_id", event.targetId },
                    { "text", event.text },
                });
            }

            const auto payload = context.dump();
            logger::info("Resolution batch {}: {} entries, {} events, trigger {}, {} bytes",
                         batch->token.batchId, batch->entries.size(), batch->events.size(),
                         ResolutionScheduling::ToString(batch->trigger), payload.size());
            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                kResolvePrompt, kResolveVariant, payload,
                [token = batch->token](std::string response, bool success) {
                    g_resolutionScheduler.SubmitResult(token, std::move(response), success);
                });
            if (!queued) {
                g_resolutionScheduler.CancelInFlight();
                logger::warn("SkyrimNet refused resolution batch {}", batch->token.batchId);
            }
        }

        void BeginRecentEvidenceSave(const std::string& saveId)
        {
            g_recentEventRecovery.BeginSave(saveId);
            g_recentPollActiveMs = 0;
            g_recentPollBaseline = false;
            g_recentPollLastMilliseconds = 0.0;
            g_recentPollTailEvents = 0;
            g_recentPollRecoveredEvents = 0;
            g_recentPollAttemptedBaseline = false;
        }

        std::vector<SkyrimNetAPI::RawDialogueEvent> PollRecentEvidence(
            bool forcePoll, const GameSnapshot& snap)
        {
            const auto nowMs = NowMs();
            const auto elapsedMs = g_recentPollTickMs == 0
                                       ? 0
                                       : std::clamp(nowMs - g_recentPollTickMs, std::int64_t{ 0 },
                                                    std::chrono::milliseconds{ kPassInterval * 2 }.count());
            g_recentPollTickMs = nowMs;
            const bool active = !snap.gamePaused && snap.windowActive;
            if (active) {
                g_recentPollActiveMs += elapsedMs;
            }
            const auto intervalMs = std::chrono::milliseconds{ kRecentEventPollInterval }.count();
            const bool baselineDue = !g_recentPollBaseline && !g_recentPollAttemptedBaseline;
            if (!active || !SkyrimNetAPI::IsMemorySystemReady() ||
                (!forcePoll && !baselineDue && g_recentPollActiveMs < intervalMs)) {
                return {};
            }
            g_recentPollActiveMs = 0;

            const auto startedMs = NowMs();
            const auto payload = SkyrimNetAPI::GetRecentEvents(kPlayerFormID, kRecentEventTail, {});
            auto result = g_recentEventRecovery.Poll(payload, startedMs, UnixNowMs());
            g_recentPollAttemptedBaseline = true;
            g_recentPollLastMilliseconds = static_cast<double>(NowMs() - startedMs);
            g_recentPollTailEvents = result.tailSize;
            g_recentPollRecoveredEvents = result.events.size();
            if (!result.valid) {
                ++g_recentPollFailures;
                logger::warn("SkyrimNet recent-event recovery poll returned malformed data after {:.0f} ms",
                             g_recentPollLastMilliseconds);
                return {};
            }
            if (result.establishedBaseline) {
                g_recentPollBaseline = true;
                logger::info("SkyrimNet recent-event recovery baseline: {} event(s), {:.0f} ms, cadence {} s",
                             result.tailSize, g_recentPollLastMilliseconds,
                             std::chrono::seconds{ kRecentEventPollInterval }.count());
            } else if (!result.events.empty()) {
                logger::info("SkyrimNet recent-event recovery accepted {} missed event(s) from a {}-event tail "
                             "in {:.0f} ms",
                             result.events.size(), result.tailSize, g_recentPollLastMilliseconds);
            } else {
                logger::debug("SkyrimNet recent-event recovery: {} tail event(s), none missed, {:.0f} ms",
                              result.tailSize, g_recentPollLastMilliseconds);
            }
            return std::move(result.events);
        }

        void PumpPendingImpulses(const Settings& settings, const GameSnapshot& snap)
        {
            // No pendingBioInjection gate. Entries now outlive a speaking turn,
            // so they exist whether or not the bio block is rendered — and
            // persistence, the TTL and the resolution check all have to keep
            // running for them, or they accumulate in memory with nothing able
            // to retire them.
            PendingImpulses::SetLedgerCap(settings.ledgerEnabled && settings.ledgerSlots > 0
                                              ? static_cast<std::size_t>(settings.ledgerSlots)
                                              : 1);
            PendingImpulses::SetPartyEchoGameDays(settings.partyEchoGameDays);

            // Every configured lens, republished with it. Two reasons, and the
            // second is why rows with no slot override are published too: a slot
            // created by Clear() has to land in a ring the size its lens asks
            // for, and a lens name only counts as a ring while it is still a
            // lens — so a row renamed on the Settings page stops being a ring
            // and its old slots fall back to the shared one rather than being
            // stranded there forever, suppressing nothing.
            std::vector<PendingImpulses::LensRing> rings;
            for (const auto& lens : settings.lenses) {
                if (lens.name[0] != '\0') {
                    rings.push_back({ lens.name, static_cast<std::size_t>(std::max(lens.ledgerSlots, 0)) });
                }
            }
            PendingImpulses::SetLensRings(std::move(rings));

            // Load-on-first-sight and save-when-dirty, both keyed off SkyrimNet's
            // save ID. Running it from here rather than from kPostLoadGame
            // sidesteps the ordering question: we simply never act until
            // SkyrimNet can say which save we are in.
            const auto saveId = SkyrimNetAPI::GetSaveUniqueID();
            PendingImpulses::SyncPersistence(saveId, snap.gameDays, settings.pendingTtlGameMinutes);
            const bool saveChanged = !saveId.empty() && g_resolutionScheduler.ActiveToken().saveId != saveId;
            if (saveChanged) {
                // A callback copied while the previous save was leaving has no
                // save token of its own. Discard the transition window before
                // activating the new token; normal play cannot emit dialogue
                // while this first valid post-load snapshot is being acquired.
                SkyrimNetAPI::DrainRawDialogueEvents();
                g_resolutionScheduler.BeginSave(saveId);
                BeginRecentEvidenceSave(saveId);
            }

            auto events = SkyrimNetAPI::DrainRawDialogueEvents();
            g_recentEventRecovery.ObserveCallbacks(events);
            auto recovered = PollRecentEvidence(saveChanged, snap);
            events.insert(events.end(), std::make_move_iterator(recovered.begin()),
                          std::make_move_iterator(recovered.end()));
            for (auto& event : events) {
                const bool npcSpeech = event.type == "dialogue" || event.type == "dialogue_npc";
                const auto raisedEntry =
                    npcSpeech ? PendingImpulses::MarkFloorOwnerRaisedByIdentity(
                                    event.actorId, event.arrivalMs, snap.gameDays)
                              : std::nullopt;
                if (raisedEntry) {
                    logger::info("Observed floor-owned speech from SkyrimNet actor {} — marked entry {} "
                                 "raised with zero resolver calls",
                                 event.actorId, *raisedEntry);
                    WithState([](Status& state) { ++state.resolutionZeroCallRaises; });
                }
                g_resolutionScheduler.Enqueue({
                    std::move(event.sourceId), event.actorId, event.targetId, std::move(event.type),
                    std::move(event.text), event.arrivalMs, raisedEntry.value_or(0) });
            }
            VerifyRestoredImpulses();
            PumpResolutionCheck(settings, snap);
            auto expiryProtected = g_resolutionScheduler.QueuedFallbacks();
            if (const auto activeBatch = g_resolutionScheduler.InFlight();
                activeBatch && activeBatch->trigger == ResolutionScheduling::Trigger::PreExpiry) {
                for (const auto& item : activeBatch->entries) {
                    expiryProtected.push_back(item.entry.id);
                }
            }
            PendingImpulses::Expire(snap.gameDays, settings.pendingTtlGameMinutes, expiryProtected);

            const auto resolution = g_resolutionScheduler.Snapshot();
            WithState([&](Status& state) {
                state.floorOwners = PendingImpulses::FloorSnapshot();
                state.personalMemoryRecords = PendingImpulses::LedgerSnapshot().size();
                state.partyMemoryRecords = PendingImpulses::PartySnapshot().size();
                state.pendingImpulses = PendingImpulses::Snapshot();
                state.resolutionQueuedEvidence = resolution.queuedRaw + resolution.acceptedEvidence;
                state.resolutionEligibleEntries = resolution.eligibleEntries;
                state.resolutionBatchInFlight = resolution.batchInFlight;
                state.resolutionLastTrigger = std::string{ ResolutionScheduling::ToString(resolution.lastTrigger) };
                state.resolutionCallsAttempted = resolution.paidBatches;
                state.resolutionQueueOverflow = resolution.queueOverflow;
                state.resolutionStaleResults = resolution.staleResults;
                state.resolutionEvidenceWatermark = PendingImpulses::EvidenceSequenceWatermark();
                state.resolutionPollBaseline = g_recentPollBaseline;
                state.resolutionPollLastMilliseconds = g_recentPollLastMilliseconds;
                state.resolutionPollTailEvents = g_recentPollTailEvents;
                state.resolutionPollRecoveredEvents = g_recentPollRecoveredEvents;
                state.resolutionPollFailures = g_recentPollFailures;
            });
        }

        void Loop()
        {
            logger::info("Director: loop started (pass every {} ms, snapshot staleness limit {} ms)",
                         std::chrono::milliseconds{ kPassInterval }.count(),
                         std::chrono::milliseconds{ kSnapshotMaxAge }.count());

            while (g_running.load()) {
                std::this_thread::sleep_for(kPassInterval);
                if (!g_running.load()) {
                    break;
                }

                // Refresh the world snapshot for the *next* pass. Reading a
                // one-second-old snapshot is fine at in-game-hour cadence, and
                // it keeps every game read on the main thread.
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() { CaptureSnapshot(); });
                }

                const bool available = SkyrimNetAPI::IsAvailable();
                const auto settings = SnapshotSettings();

                GameSnapshot snap;
                WithState([&](Status& state) {
                    state.skyrimNetAvailable = available;
                    snap = state.snapshot;
                });

                LogSnapshotChanges(snap, settings.debugLog);

                const bool snapshotFresh =
                    NowMs() - g_lastCaptureMs.load() <= std::chrono::milliseconds{ kSnapshotMaxAge }.count();

                // ---- suspension ---------------------------------------------
                //
                // The interval itself needs no protection: it is measured in
                // game time, and game time stops in a menu and in the
                // background alike. What does need protecting is everything
                // real-time — the defer clock below, and the quiet reading,
                // which after a suspend describes a room that was frozen rather
                // than one that fell silent.
                static bool wasSuspended = false;

                const bool suspended = IsSuspended(snap, snapshotFresh);
                if (wasSuspended && !suspended) {
                    g_resumedAtMs.store(NowMs());
                    logger::debug("Game resumed — nothing dispatches or is delivered for {} ms",
                                  std::chrono::milliseconds{ kResumeSettle }.count());
                }
                wasSuspended = suspended;

                // Ahead of the cue pump, which reads the timestamp it stamps.
                // Not a gate of its own — a vanilla conversation is a running,
                // readable world, unlike everything IsSuspended() covers, and
                // the asks below go on being made through one. Only delivery
                // waits.
                TrackDialogueMenu(settings, snap, snapshotFresh);
                WithState([&](Status& state) {
                    state.inVanillaDialogue = InVanillaDialogue(settings, snap);
                    state.inConversation =
                        settings.deferOnConversation && InConversation(settings, state.quiet);
                });

                // Outside every gate below, including the freshness one: while
                // the game is suspended this is the only thing that runs, and
                // stopping the defer clock is exactly what it is here to do.
                if (available) {
                    PumpPendingCues(settings, snap, suspended);
                }

                // Ahead of the impulse gates, and not subject to them. One
                // shared episode drives both silent SkyrimNet combat signals
                // and optional continuous-mode ownership.
                UpdateCombat(settings, snap, snapshotFresh, available);

                // Both of these run ahead of the impulse gates and are not
                // subject to them: a pending impulse has to expire on schedule
                // whether or not another one is due, and it is rendered into her
                // bio on every line she speaks, not only on our passes.
                if (available && snap.valid && snapshotFresh) {
                    PumpQuietPoll(settings);
                    PumpPendingImpulses(settings, snap);
                }

                // Every path that declines to ask names itself. "It never
                // fires" is the failure mode a user will actually hit, and a
                // log that goes quiet is indistinguishable from a log that
                // says why — so each gate below sets `hold` instead of
                // silently continuing. NoteHold() logs only on *change*, so
                // the steady state costs one line, not one per second.
                std::string hold;

                if (!available) {
                    hold = "SkyrimNet is not available (SkyrimNet.dll did not load)";
                } else if (!snap.valid) {
                    hold = "no save loaded yet";
                } else if (!snapshotFresh) {
                    hold = "world snapshot is stale — the main thread is not running tasks "
                           "(loading screen, main menu, or a hard stall)";
                }

                if (!hold.empty()) {
                    NoteHold(hold);
                    continue;
                }

                std::vector<std::string> manualKeys;
                {
                    std::scoped_lock lock{ g_fireLock };
                    manualKeys.assign(g_fireRequests.begin(), g_fireRequests.end());
                    g_fireRequests.clear();
                }
                const bool manual = !manualKeys.empty();
                if (manual) {
                    logger::info("{} manual ask(s) requested from the UI — bypassing the clocks and gating",
                                 manualKeys.size());
                }

                // Gates that stop every lens at once. None of them touch a
                // clock: the clocks run on game time, which is already stopped
                // in a menu, so a fight or a dismissed follower delays the asks
                // that come due inside it rather than cancelling them.
                if (!manual) {
                    if (!settings.enabled) {
                        hold = "disabled in settings";
                    } else if (settings.requireFollower && snap.followers.empty()) {
                        hold = "no followers are present ('Only when a follower is present' is on)";
                    } else if (settings.skipInCombat && snap.playerInCombat) {
                        hold = "the player is in combat ('Skip while in combat' is on)";
                    } else if (suspended) {
                        hold = snap.gamePaused ? "the game is paused" : "the game window is in the background";
                    } else if (WithinResumeSettle()) {
                        hold = "the player has only just come back — letting the game settle";
                    }
                }

                // Checked here rather than with the gates above so it covers
                // the manual trigger too: the UI button bypasses gating, but
                // there is still no prompt to send.
                const auto usable = UsableLenses(settings);
                if (hold.empty() && usable.empty()) {
                    hold = "no lens is usable — each needs a prompt file name and its own switch on the Lenses tab";
                }

                if (hold.empty() && !SkyrimNetAPI::IsMemorySystemReady()) {
                    hold = "SkyrimNet's memory system is not ready yet (no save loaded, or its database is "
                           "still opening)";
                }

                if (!hold.empty()) {
                    NoteHold(hold);
                    continue;
                }

                // The whole of scheduling. Several clocks expiring together is
                // an ordinary outcome and produces several asks, not a
                // collision: asking two questions is not worse than asking one.
                auto                    due = SyncLensClocks(usable, snap.gameDays);
                std::vector<LensChoice> asking;
                if (manual) {
                    // A named lens is asked whether or not it is due; the
                    // unnamed request means the Status page's button, which has
                    // no row to have been pressed on and takes whatever is
                    // nearest due. Either way the ask stamps the clock, so a
                    // manual ask spends the next natural one — the log would
                    // otherwise say a lens asked on a cadence it did not.
                    for (const auto& key : manualKeys) {
                        if (key.empty()) {
                            if (auto next = NextLensToAsk(usable)) {
                                asking.push_back(std::move(*next));
                            } else {
                                logger::info("Asked for an impulse, but every lens is already mid-ask");
                            }
                            continue;
                        }
                        const auto it = std::ranges::find_if(
                            usable, [&](const LensChoice& lens) { return lens.key == key; });
                        if (it == usable.end()) {
                            // Switched off between the button press and now, or
                            // a row edited out from under it.
                            logger::warn("Asked for the '{}' lens by hand, but it is no longer one this pass can "
                                         "ask — check it is still ticked on the Lenses tab",
                                         key);
                            continue;
                        }
                        if (AskInFlight(key)) {
                            logger::info("The {} lens was asked for by hand, but it is already waiting on an "
                                         "answer — leaving it be",
                                         it->name.empty() ? key : it->name);
                            continue;
                        }
                        asking.push_back(*it);
                    }
                } else {
                    asking = std::move(due);
                }

                if (asking.empty()) {
                    // Not a failure and not a hold in the old sense — this is
                    // the ordinary state of a pass, and it costs nothing at all.
                    // A manual pass that resolved to nothing has already said
                    // why, per request, so it does not restate it here.
                    if (!manual) {
                        NoteHold(DescribeNextAsk(snap.gameDays));
                    }
                    continue;
                }

                NoteHold({});

                logger::info("Asking {} lens(es) at game time {} — {} follower(s) present, location '{}'{}",
                             asking.size(), FormatGameTime(snap.gameDays), snap.followers.size(),
                             snap.location.empty() ? "unknown" : snap.location,
                             manual ? ", by hand rather than on their clocks" : ", due on their clocks");

                for (const auto& lens : asking) {
                    Fire(settings, snap, manual, lens);
                }
            }

            logger::info("Director: loop stopped");
        }
    }

    void Start()
    {
        if (g_running.exchange(true)) {
            return;
        }
        g_thread = std::thread{ Loop };
    }

    void Stop()
    {
        if (!g_running.exchange(false)) {
            return;
        }
        if (g_thread.joinable()) {
            g_thread.join();
        }
    }

    void RequestFireNow(const std::string& lensKey)
    {
        std::scoped_lock lock{ g_fireLock };
        // Deduplicated per lens, like the resolution requests: the button is
        // small, the call is not, and a double-click should not cost two. An
        // unnamed request and a named one are different asks, so neither
        // swallows the other.
        if (std::ranges::find(g_fireRequests, lensKey) != g_fireRequests.end()) {
            return;
        }
        g_fireRequests.push_back(lensKey);
    }

    void RequestResolveCheck(PendingImpulses::EntryId entryId)
    {
        ResolveRequest request{ entryId };
        std::scoped_lock lock{ g_resolveRequestLock };
        if (std::ranges::find(g_resolveRequests, request) != g_resolveRequests.end()) {
            return;
        }
        g_resolveRequests.push_back(request);
    }

    std::size_t PendingResolveRequests()
    {
        std::scoped_lock lock{ g_resolveRequestLock };
        return g_resolveRequests.size();
    }

    void SetTimescale(float scale)
    {
        // Clamped rather than trusted: the slider is bounded, but this is a
        // write into a global every quest script in the game divides by, and 0
        // stops the world.
        const auto wanted = std::clamp(scale, 1.0f, 200.0f);
        auto*      tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            logger::warn("Timescale: no task interface — the game's timescale was left alone");
            return;
        }
        tasks->AddTask([wanted]() {
            auto* calendar = RE::Calendar::GetSingleton();
            if (!calendar || !calendar->timeScale) {
                logger::warn("Timescale: the game's TimeScale global is not available — nothing was changed");
                return;
            }
            const auto before = calendar->timeScale->value;
            calendar->timeScale->value = wanted;
            // Worth a line even though nothing here reads it: this mod's whole
            // cadence is in in-game minutes, so a log that does not record the
            // moment the conversion rate moved cannot explain the ask rate
            // either side of it.
            logger::info("Timescale: {:.1f} -> {:.1f} in-game minutes per real minute, set from the settings "
                         "page. It is the game's own setting and is saved with the save; AgencyEngine does not "
                         "store it or put it back at load.",
                         before, wanted);
        });
    }

    void ResetTimer()
    {
        g_continuousReset.store(true);
        g_combatEpisodeReset.store(true);
        // Belongs to the session we just left; a load is not a resume.
        g_resumedAtMs.store(0);
        g_resolutionScheduler.BeginSave({});
        {
            // These name entries from the save we just left. PendingImpulses
            // has already forgotten them, so draining these would be harmless —
            // but a queue that survives a load is a queue that eventually
            // matches a recycled FormID.
            std::scoped_lock lock{ g_resolveRequestLock };
            g_resolveRequests.clear();
        }
        {
            // The same for cues, and they are ephemeral by design: a cue lost to
            // a load costs one announcement, and whatever it was announcing is
            // still in her bio in the save that carries it.
            std::scoped_lock lock{ g_cueLock };
            g_cues.clear();
        }
        {
            // A button pressed before the load was meant for the party in the
            // save being left, not for whoever is standing in the new one.
            std::scoped_lock lock{ g_fireLock };
            g_fireRequests.clear();
        }
        WithState([](Status& state) {
            state.pendingImpulses.clear();
            // Every clock belongs to the save we just left, and game time in the
            // new one may be anywhere. They rearm from the first pass that gets
            // past the gates, so nothing asks the instant a save comes up.
            state.lensClocks.clear();
            state.inFlight = false;
            state.deliveryPending = false;
            state.deliveryHeldSeconds = 0.0;
            state.snapshot = {};
            // continuousOwned is deliberately NOT cleared here: the Director
            // needs it on its next pass to know whether it still owes SkyrimNet
            // a switch-off from before the load. It clears it once it has.
            state.continuousPending = false;
            state.gameMasterOff = false;
        });
    }
}
