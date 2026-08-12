#include "Director.h"

#include "Logging.h"
#include "PapyrusBridge.h"
#include "PendingImpulse.h"
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
        std::atomic_bool g_fireNow{ false };
        // Set on a load/new game. Combat state, and our belief about who owns
        // continuous mode, do not survive one.
        std::atomic_bool g_continuousReset{ false };
        std::atomic<std::int64_t> g_lastCaptureMs{ 0 };
        // When the game last came back from a menu or the background. 0 means
        // it has not been suspended yet this session, which is not a wait.
        std::atomic<std::int64_t> g_resumedAtMs{ 0 };

        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
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

        // A decision with its participants resolved, on its way to SkyrimNet.
        struct ImpulseDelivery
        {
            std::string   content;
            std::string   topic;
            std::string   whyNow;   // carried for the delivery log only
            std::string   speakerName;
            std::string   targetName;
            std::uint64_t speakerUuid = 0;
            std::uint64_t targetUuid = 0;
            std::uint32_t speakerFormID = 0;
            int           delivery = kPersistentEvent;
            bool          generateThought = false;
            double        gameDays = 0.0;
            std::string   lens;
            // Declared by the lens that produced it, and carried all the way to
            // the pending entry: it is what the resolution check branches on.
            bool          proposal = false;
            // That lens's ledger ring size, 0 meaning the global count.
            int           lensLedgerSlots = 0;
        };

        // A finished impulse waiting for the party to stop talking, and when it
        // started waiting. Written on the main thread by the delivery hop, read
        // and cleared by the Director thread, so it needs its own lock — the
        // Status mutex is held only for short reads and this holds strings.
        std::mutex                     g_pendingLock;
        std::optional<ImpulseDelivery> g_pending;
        std::int64_t                   g_pendingSinceMs = 0;

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
            std::uint32_t formID = 0;
            std::string   lens;

            bool operator==(const ResolveRequest&) const = default;
        };
        std::mutex                 g_resolveRequestLock;
        std::deque<ResolveRequest> g_resolveRequests;

        // Is the party quiet enough to speak into?
        //
        // Three signals sampled together in Papyrus. Every uncertain case
        // resolves to "not quiet": a late impulse costs nothing, and an early
        // one talks over the player.
        bool IsQuiet(const Settings& settings, const QuietReading& reading)
        {
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
        std::string DescribeReading(const QuietReading& reading)
        {
            if (!reading.valid) {
                return "no reading yet";
            }
            const auto sinceDialogue = SkyrimNetAPI::MsSinceLastDialogue();
            return std::format("recording={} speechQueue={} audio={}ms dialogue={}", reading.recording,
                               reading.speechQueue, reading.msSinceAudioEnded,
                               sinceDialogue < 0 ? std::string{ "none seen" } : std::format("{}ms", sinceDialogue));
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
            // The two declared properties that decide what happens to the
            // impulse afterwards. Carried from the settings row rather than
            // matched on the name later, because the name is the one field the
            // user can edit freely.
            bool        proposal = false;
            int         ledgerSlots = 0;
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
                                          std::max(lens.intervalGameMinutes, 1.0f),
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
                    (countAsSpoken ? tally->spoken : tally->quiet) += 1;
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

        // Asks the speaker to think privately about what they just decided to
        // bring up. The thought lands in their own event history, which is
        // where this mod's prompt reads thoughts back from — so it is the only
        // thing in the loop that carries forward to the next impulse.
        //
        // Fired after delivery, and deliberately not waited on: generation is
        // asynchronous, so it will land well after the speaking turn. It
        // colours everything they say from the next call onward, not this one.
        void SeedThought(const ImpulseDelivery& d)
        {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(d.speakerFormID);
            if (!actor) {
                logger::warn("Wanted a thought from {} ({:08X}) but they are no longer loaded — skipping",
                             d.speakerName, d.speakerFormID);
                return;
            }

            // Second person, matching the hint style SkyrimNet's own examples
            // use ("The Jarl insulted your honor — how do you feel?").
            const auto hint = std::format(
                "You have just decided to bring something up with {}, unprompted: {} What is going through your "
                "head as you say it, and what do you want out of it?",
                d.targetName, d.content);

            if (PapyrusBridge::GenerateNPCThought(actor, hint)) {
                logger::info("Asked SkyrimNet for a private thought from {} about what they just raised", d.speakerName);
            }
        }

        // The recorded delivery: the impulse becomes something the companion is
        // privately chewing on rather than something she says.
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
                logger::warn("Wanted to record {}'s impulse as a thought, but they are no longer loaded ({:08X}) — "
                             "the impulse is lost",
                             d.speakerName, d.speakerFormID);
                return false;
            }

            // Framed as unsaid on purpose. SeedThought's hint says "you have just
            // decided to bring this up", which is true after a speaking turn and
            // false here — this delivery is the one that is never voiced.
            const auto hint = std::format(
                "Something about {} is sitting with you that you have NOT said out loud, and are not saying now: {} "
                "What are you actually thinking about it, privately?",
                d.targetName, d.content);

            return PapyrusBridge::GenerateNPCThought(actor, hint);
        }

        // The recorded delivery, done properly: the impulse is held in the DLL
        // and reaches her next prompt through the decorator SkyrimNet calls
        // while rendering her character bio — verbatim, privately, with no LLM
        // call in between.
        //
        // This supersedes RecordAsThought as the carrier of the *agenda*. The
        // generated thought is still worth having as her private *reaction* to
        // carrying it, so both run when 'generateThought' is on; the thought is
        // paraphrase and colour, this is the text.
        bool RecordAsPendingImpulse(const ImpulseDelivery& d)
        {
            if (d.speakerFormID == 0) {
                logger::warn("Wanted to record {}'s impulse for her bio, but there is no FormID to key it to — the "
                             "impulse is lost",
                             d.speakerName);
                return false;
            }

            PendingImpulses::Entry entry;
            entry.formID = d.speakerFormID;
            entry.speakerName = d.speakerName;
            entry.targetName = d.targetName;
            entry.text = d.content;
            entry.topic = d.topic;
            entry.lens = d.lens;
            entry.proposal = d.proposal;
            entry.createdGameDays = d.gameDays;
            PendingImpulses::Set(std::move(entry));
            return true;
        }

        // The spoken counterpart. She has just said it, so the entry is created
        // (or replaced) already marked spoken — her bio switches to the "raised
        // and unanswered" wording, and the resolution check starts asking the
        // other question about it.
        //
        // This is where the old behaviour changed: dispatch used to Clear the
        // entry outright, on the reasoning that there was no way to tell "the
        // same impulse" from "a different one about the same person". The topic
        // field is that way. Clearing also meant a spoken beat was never checked
        // for having landed, so every one was treated as settled by assumption —
        // which is most of why the same subject came back days later.
        void RecordSpokenImpulse(const ImpulseDelivery& d, int ledgerSlots, bool ledgerEnabled)
        {
            if (d.speakerFormID == 0) {
                return;
            }

            PendingImpulses::Entry entry;
            entry.formID = d.speakerFormID;
            entry.speakerName = d.speakerName;
            entry.targetName = d.targetName;
            entry.text = d.content;
            entry.topic = d.topic;
            entry.lens = d.lens;
            entry.proposal = d.proposal;
            entry.createdGameDays = d.gameDays;
            entry.spoken = true;
            entry.spokenGameDays = d.gameDays;
            PendingImpulses::Set(std::move(entry));

            // The lens's own count decides whether there is a ring to record
            // into; the global one only stands in for a lens that asks for it.
            // Gating on the global alone meant a lens configured with three
            // slots recorded nothing whenever the global was 0.
            const auto slots = d.lensLedgerSlots > 0 ? d.lensLedgerSlots : ledgerSlots;
            if (ledgerEnabled && slots > 0 && !d.topic.empty()) {
                // The slot goes in this lens's own ring, sized by that lens's
                // count — 0 there means the global one. A lens only ever
                // displaces its own subjects, so a lens that cycles fast cannot
                // quietly release what another one settled.
                PendingImpulses::LedgerRecord(d.speakerFormID, d.speakerName, d.topic, d.lens,
                                              static_cast<std::size_t>(std::max(d.lensLedgerSlots, 0)));
                logger::info("Ledger: {} has raised '{}' under the {} lens — held until we know whether anyone met it",
                             d.speakerName, OneLine(d.topic), d.lens.empty() ? "unnamed" : d.lens);
            }
        }

        // Writes the finished impulse into SkyrimNet. Main thread — the Papyrus
        // VM is not thread-safe.
        void DeliverImpulse(ImpulseDelivery d)
        {
            const auto settings = SnapshotSettings();

            bool ok = false;
            std::string how;

            // Direct narration is what actually gets the companion talking, and
            // it needs a UUID to speak as. Without one there is nobody to hand
            // the stage direction to, so it degrades to an event the party
            // merely knows about.
            if (d.delivery == kDirectNarration && d.speakerUuid != 0) {
                how = "direct narration";
                ok = PapyrusBridge::DirectNarration(d.content, d.speakerUuid, d.targetUuid);

                // She has just opened her mouth unprompted. The subject does not
                // stop existing here — it stops being *unsaid*. Whatever she was
                // carrying is superseded by what she actually said (Set replaces
                // per actor), now marked spoken, and the topic takes a
                // provisional ledger slot so the next ask does not propose it
                // straight back.
                if (ok) {
                    RecordSpokenImpulse(d, settings.ledgerSlots, settings.ledgerEnabled);
                }
            } else if (settings.pendingBioInjection) {
                how = "recorded impulse";
                ok = RecordAsPendingImpulse(d);
                if (ok && settings.generateThought) {
                    // Two different things about one impulse: the pending entry
                    // is the agenda, in her words-to-be; this is what carrying
                    // it feels like. RecordAsThought's hint is already framed as
                    // unsaid, which is exactly right here.
                    how = "recorded impulse + thought";
                    RecordAsThought(d);
                }
            } else {
                how = "generated thought";
                ok = RecordAsThought(d);
            }

            if (ok) {
                logger::info("{} raises something with {} — delivered as a {} at game time {}: {}", d.speakerName,
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

            // Only on the spoken path, and only when it actually reached
            // SkyrimNet. SeedThought's hint says "you have just decided to bring
            // this up" — true after a speaking turn and false after every other
            // delivery here, each of which has already arranged its own private
            // half above. Seeding on top of those would be two LLM calls
            // producing two thoughts about one impulse, the second of them
            // claiming she said something she never said.
            const bool spokenAloud = (how == "direct narration");
            if (ok && spokenAloud && d.generateThought) {
                SeedThought(d);
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

        // Where the conversation check actually belongs. Main thread.
        //
        // Checking before dispatch would protect nothing: the LLM round trip
        // runs 4-8 seconds, so the party's state at request time says nothing
        // about their state when the answer lands. Delivery is instantaneous,
        // so this is the only point where "are they talking right now" and
        // "are we about to interrupt them" are the same question.
        void DeliverOrHold(ImpulseDelivery d)
        {
            const auto settings = SnapshotSettings();

            // A persistent event is never voiced, so it interrupts nothing and
            // has no reason to wait.
            if (d.delivery != kDirectNarration) {
                DeliverImpulse(std::move(d));
                return;
            }

            QuietReading reading;
            WithState([&](Status& state) { reading = state.quiet; });
            const bool suspended = IsSuspended();

            // The dispatch gate cleared several seconds ago; a menu opened or
            // the window went to the background in the meantime. Speaking now
            // spends the line on an empty room, and the player hears it the
            // instant they come back. This holds regardless of
            // deferOnConversation — that setting is about talking over people,
            // this is about talking to nobody.
            const bool waiting = suspended || WithinResumeSettle();

            if (!waiting && (!settings.deferOnConversation || IsQuiet(settings, reading))) {
                DeliverImpulse(std::move(d));
                return;
            }

            if (waiting) {
                logger::info("{} has something to raise with {}, but the game is {} — holding it until the player "
                             "is back",
                             d.speakerName, d.targetName, suspended ? "paused or in the background" : "just back");
            } else {
                logger::info(
                    "{} has something to raise with {}, but the party is talking — holding it (up to {:.0f}s). "
                    "Reading: {}, threshold {:.0f}s",
                    d.speakerName, d.targetName, settings.maxDeferSeconds, DescribeReading(reading),
                    settings.quietSeconds);
            }

            {
                std::scoped_lock lock{ g_pendingLock };
                g_pending = std::move(d);
                g_pendingSinceMs = NowMs();
            }
            WithState([](Status& state) { state.deliveryPending = true; });
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
            context["tail_live"] = !manual && !IsQuiet(settings, reading);

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
                            nowGameDays, false, false });
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

            const auto delivery = settings.delivery;
            const auto gameDays = snap.gameDays;

            StampAsk(lens, gameDays);
            WithState([&](Status& state) {
                state.lastError.clear();
                state.lastContextJson = contextJson;
            });

            // The LLM variant stays the same across lenses on purpose: they are
            // the same job at the same cost, and one variant means one place in
            // SkyrimNet's UI to point impulses at a cheaper model.
            logger::info("Asking the {} question — prompt '{}' (variant '{}'), context {} bytes, {} follower(s), "
                         "delivery '{}'. Next {} ask in {:.0f} in-game minutes, or {:.0f} if this one carries",
                         lens.name.empty() ? "unnamed" : lens.name, lens.prompt, kLLMVariant, contextJson.size(),
                         roster.size(), delivery == kDirectNarration ? "direct-narration" : "persistent-event",
                         lens.name.empty() ? "unnamed" : lens.name, lens.intervalGameMinutes,
                         lens.intervalGameMinutes + lens.cooldownGameMinutes);

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
                // game — the delivery hop is posted to the main thread.
                [delivery, gameDays, dispatchedAt, player = std::move(player), roster = std::move(roster),
                 generateThought = settings.generateThought, lensKey = lens.key, lensName = lens.name,
                 proposal = lens.proposal, lensLedgerSlots = lens.ledgerSlots,
                 interval = lens.intervalGameMinutes, cooldown = lens.cooldownGameMinutes,
                 ledgerVeto = settings.ledgerVeto && settings.ledgerEnabled,
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

                    // Target defaults to the player. A companion turning to
                    // another companion is the whole point of seeing the party
                    // as a party, so it only resolves that way when the model
                    // actually named someone else who is here.
                    const Participant* target = &player;
                    if (!decision.target.empty() && !IEquals(decision.target, player.name)) {
                        if (const auto* named = FindParticipant(roster, decision.target);
                            named && named != speaker) {
                            target = named;
                        } else if (!named) {
                            logger::warn("The model named '{}' as the target, who is not present — addressing {} "
                                         "instead",
                                         decision.target, player.name);
                        }
                    }

                    if (delivery == kDirectNarration && speaker->uuid == 0) {
                        logger::warn("SkyrimNet has no UUID for {} — they cannot be given a speaking turn, so this "
                                     "lands as a persistent event instead",
                                     speaker->name);
                    }

                    // The backstop. The prompt is shown what she has already
                    // raised and told not to repeat it; this catches the times
                    // it does anyway. The call is already spent by now, so the
                    // saving is not tokens — it is that she does not say the
                    // same thing twice, and that the log names which subject was
                    // held back, which is the only way to tell a prompt that is
                    // being ignored from one that is working.
                    // Scoped to this lens's ring, like eviction: another lens
                    // holding the subject is not this lens's business, and the
                    // prompt already renders every lens's slots as one "already
                    // raised" list so a repeat across lenses is headed off
                    // before the call rather than after it.
                    if (ledgerVeto && !decision.topic.empty() &&
                        PendingImpulses::LedgerSuppresses(speaker->formID, decision.topic, lensName)) {
                        logger::info("Held back: {} has already raised '{}' under the {} lens and the ledger still "
                                     "holds it. Nothing is carried from this ask.",
                                     speaker->name, OneLine(decision.topic), lensName.empty() ? "unnamed" : lensName);
                        Impulse held;
                        held.when = FormatGameTime(gameDays);
                        held.content = std::format("(held back: {} has already raised '{}')", speaker->name,
                                                   OneLine(decision.topic));
                        held.speaker = speaker->name;
                        held.topic = decision.topic;
                        held.delivery = "held (already raised)";
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
                    outgoing.delivery = delivery;
                    outgoing.generateThought = generateThought;
                    outgoing.gameDays = gameDays;
                    outgoing.lens = lensName;
                    outgoing.proposal = proposal;
                    outgoing.lensLedgerSlots = lensLedgerSlots;

                    // The ask produced something to carry, so this lens goes
                    // quiet for the cooldown as well as the interval. Stamped
                    // here rather than at delivery: delivery can be held for a
                    // conversation, and a lens that stayed askable in the
                    // meantime could produce a second impulse about the same
                    // day for the same companion.
                    logger::info("The {} lens carried something, so it goes quiet for {:.0f} in-game minutes",
                                 lensName.empty() ? "unnamed" : lensName, interval + cooldown);
                    release(true);

                    SKSE::GetTaskInterface()->AddTask(
                        [outgoing = std::move(outgoing)]() mutable { DeliverOrHold(std::move(outgoing)); });
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

            if (verbose) {
                logger::trace(
                    "Snapshot: valid={} gameTime={} location='{}' inCombat={} paused={} windowActive={} followers={}",
                    snap.valid, FormatGameTime(snap.gameDays), snap.location, snap.playerInCombat, snap.gamePaused,
                    snap.windowActive, snap.followers.size());
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
        }

        // ---- SkyrimNet continuous mode, held for the length of a fight -----
        //
        // Called on every pass from the Director thread, ahead of and independent
        // of the impulse gates: whether an impulse is due has nothing to do with
        // whether the party should be talking through a fight.
        //
        // The whole read-modify-write lives in Papyrus (AgencyEngine_Bridge),
        // so everything here is fire-and-forget; ownership comes back through
        // the mod-event sink, which is why this reads it out of Status rather
        // than tracking it locally.
        void UpdateContinuousMode(const Settings& settings, const GameSnapshot& snap, bool snapshotFresh)
        {
            // Director thread only, like the other helpers in this section.
            static bool         active = false;      // we consider a fight in progress
            static std::int64_t leftCombatMs = 0;    // when combat last dropped, 0 = not counting
            static int          acquireReports = -1; // report count at acquire, -1 = nothing outstanding
            static std::int64_t acquiredMs = 0;

            bool owned = false;
            int  reports = 0;
            WithState([&](Status& state) {
                owned = state.continuousOwned;
                reports = state.continuousReports;
            });

            const auto release = [&](const char* why) {
                logger::info("Releasing continuous mode: {}", why);
                WithState([](Status& state) {
                    state.continuousOwned = false;
                    state.continuousPending = true;
                });
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() { PapyrusBridge::SetContinuousMode(false); });
                }
            };

            if (g_continuousReset.exchange(false)) {
                // A load lands us anywhere — a save from before this fight, or
                // another one entirely — so the combat edge we were tracking is
                // meaningless now. Continuous mode itself is not save data
                // though: it's SkyrimNet's own runtime setting, still on from
                // before the load. Hand it back here or nothing ever will.
                if (owned) {
                    release("a save was loaded while we were holding it");
                    owned = false;
                }
                active = false;
                leftCombatMs = 0;
                acquireReports = -1;
            }

            if (!settings.combatContinuousMode) {
                // Turned off mid-fight. Hand back anything we are holding rather
                // than leaving the player in a mode they just asked to stop using.
                if (active && owned) {
                    release("the setting was switched off while we were holding it");
                }
                active = false;
                leftCombatMs = 0;
                acquireReports = -1;
                return;
            }

            // A stale snapshot means the main thread isn't running our tasks, so
            // playerInCombat is whatever it was before the stall — acting on it
            // would switch the mode on the strength of an old reading. Holding
            // position is the safe answer: if we own the mode we keep owning it.
            if (!snap.valid || !snapshotFresh) {
                return;
            }

            if (snap.playerInCombat) {
                leftCombatMs = 0;
                if (active) {
                    return;
                }
                active = true;
                acquireReports = reports;
                acquiredMs = NowMs();
                logger::info("Combat started — asking for continuous mode");
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    WithState([](Status& state) { state.continuousPending = true; });
                    tasks->AddTask([]() { PapyrusBridge::SetContinuousMode(true); });
                }
                return;
            }

            if (!active) {
                return;
            }

            // Out of combat. Wait out the grace period before switching back:
            // IsInCombat() drops between waves, and each flip is a HUD
            // notification the player has to watch.
            if (leftCombatMs == 0) {
                leftCombatMs = NowMs();
                logger::debug("Combat ended — {:.0f}s grace before continuous mode is released",
                              settings.continuousExitGraceSeconds);
                return;
            }
            const auto graceMs = static_cast<std::int64_t>(settings.continuousExitGraceSeconds * 1000.0f);
            if (NowMs() - leftCombatMs < graceMs) {
                return;
            }

            // A fight shorter than the Papyrus round trip would otherwise be
            // released before we knew whether we owned the mode — and "not
            // owned" reads identically to "the player already had it on", so we
            // would leave it switched on forever. Wait for the report, but not
            // indefinitely: if it never arrives the helper isn't running, and
            // there is nothing to release anyway.
            if (acquireReports >= 0 && reports == acquireReports) {
                if (NowMs() - acquiredMs < 5000) {
                    return;
                }
                logger::warn("No reply from AgencyEngine_Bridge 5s after asking for continuous mode — giving up on "
                             "this fight. Is Scripts/AgencyEngine_Bridge.pex installed?");
            }

            active = false;
            leftCombatMs = 0;
            acquireReports = -1;

            if (owned) {
                release("combat ended");
            } else {
                logger::debug("Combat ended, but continuous mode isn't ours to switch off — leaving it as it is");
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

        // Delivers a held impulse the moment the party goes quiet, or gives up
        // on it. Director thread.
        // Called on every pass, including the ones where the world is not
        // readable — that is the whole point. This is the only thing in the
        // file whose clock has to keep being *stopped* while the game is
        // suspended, and it cannot do that from behind a freshness guard.
        void PumpPendingDelivery(const Settings& settings, bool suspended)
        {
            static std::int64_t lastPumpMs = 0;

            const auto now = NowMs();
            const auto sinceLastPump = lastPumpMs == 0 ? 0 : now - lastPumpMs;
            lastPumpMs = now;

            std::int64_t heldMs = 0;
            {
                std::scoped_lock lock{ g_pendingLock };
                if (!g_pending) {
                    return;
                }
                // The defer clock is real time, and being suspended does not
                // stop real time. Left alone, a two-minute alt-tab exhausts
                // maxDeferSeconds against a party that was never talking, and
                // the impulse is downgraded (or dropped) for a conversation
                // that never happened. Push the start forward instead, so
                // suspended time is not held time.
                if (suspended) {
                    g_pendingSinceMs += sinceLastPump;
                }
                heldMs = now - g_pendingSinceMs;
            }

            WithState([&](Status& state) {
                state.deliveryHeldSeconds = static_cast<double>(heldMs) / 1000.0;
            });

            // Nobody is there to hear it, or they have only just sat back down.
            // The quiet reading is not consulted at all here: after a suspend it
            // describes a room that was frozen, not one that fell silent.
            if (suspended || WithinResumeSettle()) {
                return;
            }

            QuietReading reading;
            WithState([&](Status& state) { reading = state.quiet; });

            // With conversation deferral off, the party's state is not a reason
            // to wait — the only thing that put this in the queue was the game
            // being suspended, and it no longer is.
            const bool quiet = !settings.deferOnConversation || IsQuiet(settings, reading);
            const bool expired = heldMs >= static_cast<std::int64_t>(settings.maxDeferSeconds * 1000.0f);
            if (!quiet && !expired) {
                return;
            }

            ImpulseDelivery outgoing;
            {
                std::scoped_lock lock{ g_pendingLock };
                if (!g_pending) {
                    return;
                }
                outgoing = std::move(*g_pending);
                g_pending.reset();
            }
            WithState([](Status& state) {
                state.deliveryPending = false;
                state.deliveryHeldSeconds = 0.0;
            });

            if (quiet) {
                logger::info("The party went quiet after {:.0f}s — {} raises it now. Reading: {}", heldMs / 1000.0,
                             outgoing.speakerName, DescribeReading(reading));
            } else if (settings.degradeToPersistentEvent) {
                // The impulse was written against state that is now up to a
                // minute old, and the conversation has moved on. Recording it
                // instead of speaking it is the graceful failure: she does not
                // interrupt, the topic lands in her context, and it colours
                // whatever she says next through SkyrimNet's own loop.
                logger::info("Still talking after {:.0f}s — recording {}'s impulse as a persistent event instead of "
                             "interrupting",
                             heldMs / 1000.0, outgoing.speakerName);
                outgoing.delivery = kPersistentEvent;
            } else {
                logger::info("Still talking after {:.0f}s — dropping {}'s impulse", heldMs / 1000.0,
                             outgoing.speakerName);
                Impulse dropped;
                dropped.when = FormatGameTime(outgoing.gameDays);
                dropped.content = outgoing.content;
                dropped.speaker = outgoing.speakerName;
                dropped.target = outgoing.targetName;
                dropped.delivery = "dropped (the party never stopped talking)";
                dropped.lens = outgoing.lens;
                dropped.ok = true;
                RecordImpulse(std::move(dropped), false);
                return;
            }

            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([outgoing = std::move(outgoing)]() mutable { DeliverImpulse(std::move(outgoing)); });
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
                        PendingImpulses::ClearAll(entry.formID,
                                                  "stale (no actor with that FormID — load order changed)");
                        continue;
                    }
                    const char* name = actor->GetDisplayFullName();
                    if (!name || entry.speakerName != name) {
                        PendingImpulses::ClearAll(entry.formID,
                                                  std::format("stale ({:08X} is now '{}', not '{}')", entry.formID,
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
        //
        // The TTL alone is not enough: the common way an agenda stops being live
        // is that the conversation covered it, which no clock can see. Without
        // this, her bio goes on saying she has been meaning to raise something
        // she raised an hour ago — and there is no signal in the loop that would
        // ever notice.
        void PumpResolutionCheck(const Settings& settings, const GameSnapshot& snap)
        {
            // Cleared from a SkyrimNet worker, so not a plain bool. One check in
            // flight at a time: they are cheap but not free, and a stall would
            // otherwise queue one per pass.
            static std::atomic_bool checkInFlight{ false };

            // One at a time, whether asked for by hand or by the clock. Checked
            // before the queue is touched so a manual request stays queued
            // rather than being dropped while another check is out.
            if (checkInFlight.load()) {
                return;
            }

            // A manual request beats the cadence and outranks anything merely
            // due, because someone is sitting in the menu waiting for it.
            std::optional<PendingImpulses::Entry> due;
            bool                                  manual = false;
            {
                const auto       live = PendingImpulses::Snapshot();
                std::scoped_lock lock{ g_resolveRequestLock };
                while (!g_resolveRequests.empty() && !due) {
                    const auto request = g_resolveRequests.front();
                    g_resolveRequests.pop_front();
                    // It may have been cleared — by hand, by the TTL, or by a
                    // scheduled check — between the button press and now.
                    for (const auto& entry : live) {
                        if (entry.formID == request.formID && entry.lens == request.lens && !entry.unverified) {
                            due = entry;
                            manual = true;
                            break;
                        }
                    }
                }
            }

            if (!due) {
                // Deliberately not gated on pendingBioInjection any more. Spoken
                // entries are created on the narration path whether or not the
                // bio block is switched on, and they are what confirms a ledger
                // slot — gate the check on the bio setting and turning the bio
                // off would silently mean no slot is ever confirmed, every one
                // withdrawn at TTL, and the ledger degrades to nothing with no
                // sign of why.
                if (settings.pendingResolveGameMinutes <= 0.0f) {
                    return;
                }
                due = PendingImpulses::NextDueForCheck(snap.gameDays, settings.pendingResolveGameMinutes);
            }
            if (!due) {
                return;
            }
            // Stamped at dispatch rather than at the answer: a check that never
            // comes back must not retry every second.
            PendingImpulses::MarkChecked(due->formID, due->lens, snap.gameDays);

            nlohmann::json context;
            context["npc_name"] = due->speakerName;
            context["target_name"] = due->targetName;
            context["impulse"] = due->text;
            context["game_time"] = FormatGameTime(snap.gameDays);
            // Which question the prompt asks. When she has said it, her own
            // raise is the premise and cannot be evidence that it landed —
            // counting it would mark every beat settled the moment she opened
            // her mouth, which is exactly the state this is here to notice.
            context["spoken"] = due->spoken;
            // Which of the two questions "has it been met?" means. For a topic,
            // an answer of any kind meets it. For a proposal, agreement is a
            // deferral: it is met when the events show the thing happened, or
            // when someone plainly refused. Always set — an unset name is a
            // render error in Inja, and a render error costs the check.
            context["proposal"] = due->proposal;
            context["marked_at"] = FormatGameTime(due->spoken ? due->spokenGameDays : due->createdGameDays);
            // Her own tail, not the player's: the question is whether *she* has
            // had this out, and a generous window because the whole point is to
            // catch an exchange that happened while we were not looking.
            context["recent_events"] = ParseEvents(SkyrimNetAPI::GetRecentEvents(
                due->formID, std::max(settings.perFollowerEvents, 30), settings.eventTypeFilter));

            const auto payload = context.dump();
            logger::info("Asking whether {}'s {} impulse is still live ({}, {} bytes of context)", due->speakerName,
                         due->lens.empty() ? "unnamed" : due->lens,
                         manual ? "asked for from the UI" : "on the in-game cadence", payload.size());

            checkInFlight.store(true);
            const bool queued = SkyrimNetAPI::SendCustomPromptToLLM(
                kResolvePrompt, kResolveVariant, payload,
                // SkyrimNet worker thread. Touches only PendingImpulses, which
                // has its own lock — no game objects, so no main-thread hop.
                [formID = due->formID, lens = due->lens, name = due->speakerName,
                 wasSpoken = due->spoken](std::string response, bool success) {
                    checkInFlight.store(false);
                    if (!success) {
                        logger::warn("Resolution check for {} failed — leaving the impulse pending: {}", name,
                                     response.empty() ? "(no error text)" : OneLine(Elide(response)));
                        return;
                    }

                    // Deliberately conservative in both directions of doubt:
                    // only an explicit, parseable yes clears it. A malformed
                    // answer leaves the impulse pending, where the TTL will
                    // still reach it — the alternative is a check that silently
                    // eats agendas whenever the model drifts off format.
                    bool resolved = false;
                    const auto text = Unfence(response);
                    const auto open = text.find('{');
                    const auto close = text.rfind('}');
                    if (open != std::string::npos && close != std::string::npos && close > open) {
                        try {
                            const auto parsed = nlohmann::json::parse(text.substr(open, close - open + 1));
                            if (parsed.is_object() && parsed.contains("resolved")) {
                                const auto& value = parsed["resolved"];
                                if (value.is_boolean()) {
                                    resolved = value.get<bool>();
                                } else if (value.is_string()) {
                                    const auto spelled = value.get<std::string>();
                                    resolved = IEquals(spelled, "true") || IEquals(spelled, "yes");
                                }
                            }
                        } catch (const std::exception&) {
                            // Falls through to "still live", which is the safe
                            // answer — see above.
                        }
                    }

                    if (resolved) {
                        // The only place anything is judged settled, and so the
                        // only place a ledger slot is made durable. Every other
                        // way an entry dies means the subject was never answered
                        // and should be raisable again, which is what the
                        // default Withdraw does.
                        PendingImpulses::Clear(formID, lens,
                                               wasSpoken ? "resolved (what she raised was met)"
                                                         : "resolved (it was had out without her)",
                                               PendingImpulses::Disposition::Confirm);
                    } else {
                        logger::debug("{}'s {} impulse is still live", name, wasSpoken ? "spoken" : "pending");
                    }
                });

            if (!queued) {
                checkInFlight.store(false);
                logger::warn("SkyrimNet refused the resolution check for {} — it stays pending", due->speakerName);
            }
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
            PendingImpulses::SyncPersistence(SkyrimNetAPI::GetSaveUniqueID(), snap.gameDays,
                                             settings.pendingTtlGameMinutes);
            VerifyRestoredImpulses();
            PendingImpulses::ExpireOlderThan(snap.gameDays, settings.pendingTtlGameMinutes);
            PumpResolutionCheck(settings, snap);

            WithState([](Status& state) { state.pendingImpulses = PendingImpulses::Snapshot(); });
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

                // Outside every gate below, including the freshness one: while
                // the game is suspended this is the only thing that runs, and
                // stopping the defer clock is exactly what it is here to do.
                if (available) {
                    PumpPendingDelivery(settings, suspended);
                }

                // Ahead of the impulse gates, and not subject to them: holding
                // continuous mode through a fight is a separate job from
                // deciding whether anyone has something to raise. It needs
                // SkyrimNet loaded, and nothing else.
                if (available) {
                    UpdateContinuousMode(settings, snap, snapshotFresh);
                }

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

                const bool manual = g_fireNow.exchange(false);
                if (manual) {
                    logger::info("Manual trigger requested from the UI — bypassing the clocks and gating");
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
                    if (auto next = NextLensToAsk(usable)) {
                        asking.push_back(std::move(*next));
                    }
                } else {
                    asking = std::move(due);
                }

                if (asking.empty()) {
                    // Not a failure and not a hold in the old sense — this is
                    // the ordinary state of a pass, and it costs nothing at all.
                    NoteHold(manual ? std::string{ "every lens is mid-ask" } : DescribeNextAsk(snap.gameDays));
                    continue;
                }

                NoteHold({});

                logger::info("{} lens(es) due at game time {} — {} follower(s) present, location '{}'{}",
                             asking.size(), FormatGameTime(snap.gameDays), snap.followers.size(),
                             snap.location.empty() ? "unknown" : snap.location,
                             manual ? ", manually triggered" : "");

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

    void RequestFireNow()
    {
        g_fireNow.store(true);
    }

    void RequestResolveCheck(std::uint32_t formID, const std::string& lens)
    {
        ResolveRequest request{ formID, lens };
        std::scoped_lock lock{ g_resolveRequestLock };
        // Deduplicated: the button is small, the call is not, and a double-click
        // should not cost two. Two impulses from the same companion are two
        // requests, though — different questions with different answers.
        if (std::ranges::find(g_resolveRequests, request) != g_resolveRequests.end()) {
            return;
        }
        g_resolveRequests.push_back(std::move(request));
    }

    std::size_t PendingResolveRequests()
    {
        std::scoped_lock lock{ g_resolveRequestLock };
        return g_resolveRequests.size();
    }

    void ResetTimer()
    {
        g_continuousReset.store(true);
        // Belongs to the session we just left; a load is not a resume.
        g_resumedAtMs.store(0);
        {
            // These name entries from the save we just left. PendingImpulses
            // has already forgotten them, so draining these would be harmless —
            // but a queue that survives a load is a queue that eventually
            // matches a recycled FormID.
            std::scoped_lock lock{ g_resolveRequestLock };
            g_resolveRequests.clear();
        }
        WithState([](Status& state) {
            state.pendingImpulses.clear();
            // Every clock belongs to the save we just left, and game time in the
            // new one may be anywhere. They rearm from the first pass that gets
            // past the gates, so nothing asks the instant a save comes up.
            state.lensClocks.clear();
            state.inFlight = false;
            state.snapshot = {};
            // continuousOwned is deliberately NOT cleared here: the Director
            // needs it on its next pass to know whether it still owes SkyrimNet
            // a switch-off from before the load. It clears it once it has.
            state.continuousPending = false;
            state.gameMasterOff = false;
        });
    }
}
