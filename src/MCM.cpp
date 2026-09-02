#include "MCM.h"

#include "Director.h"
#include "PendingImpulse.h"
#include "Settings.h"
#include "State.h"

namespace AgencyEngine::MCM
{
    namespace
    {
        constexpr auto kScriptName = "AgencyEngine_MCMNative"sv;

        struct BoolField
        {
            std::string_view name;
            bool Settings::* member;
        };

        struct IntField
        {
            std::string_view name;
            int Settings::*  member;
            int              minimum;
            int              maximum;
        };

        struct FloatField
        {
            std::string_view name;
            float Settings::* member;
            float             minimum;
            float             maximum;
        };

        inline constexpr BoolField kBoolFields[] = {
            { "enabled", &Settings::enabled },
            { "cues", &Settings::cues },
            { "generateThought", &Settings::generateThought },
            { "requireFollower", &Settings::requireFollower },
            { "skipInCombat", &Settings::skipInCombat },
            { "ledgerEnabled", &Settings::ledgerEnabled },
            { "ledgerVeto", &Settings::ledgerVeto },
            { "combatContinuousMode", &Settings::combatContinuousMode },
            { "combatEventsEnabled", &Settings::combatEventsEnabled },
            { "deferOnConversation", &Settings::deferOnConversation },
            { "injectQuietGap", &Settings::injectQuietGap },
            { "debugLog", &Settings::debugLog },
        };

        inline constexpr IntField kIntFields[] = {
            { "maxEvents", &Settings::maxEvents, 5, 200 },
            { "perFollowerEvents", &Settings::perFollowerEvents, 0, 120 },
            { "forcedImpulseChance", &Settings::forcedImpulseChance, 0, 100 },
            { "ledgerSlots", &Settings::ledgerSlots, 1, 20 },
            { "pendingResolveEventInterval", &Settings::pendingResolveEventInterval, 1, 200 },
        };

        inline constexpr FloatField kFloatFields[] = {
            { "pendingTtlGameMinutes", &Settings::pendingTtlGameMinutes, 30.0f, 4320.0f },
            { "partyEchoGameDays", &Settings::partyEchoGameDays, 0.0f, 30.0f },
            { "continuousExitGraceSeconds", &Settings::continuousExitGraceSeconds, 0.0f, 60.0f },
            { "combatEventIntervalSeconds", &Settings::combatEventIntervalSeconds, 5.0f, 120.0f },
            { "quietSeconds", &Settings::quietSeconds, 0.0f, 60.0f },
            { "conversationSettleSeconds", &Settings::conversationSettleSeconds, 0.0f, 300.0f },
            { "maxDeferSeconds", &Settings::maxDeferSeconds, 5.0f, 300.0f },
            { "quietPollSeconds", &Settings::quietPollSeconds, 0.25f, 5.0f },
            { "pendingResolveCooldownSeconds", &Settings::pendingResolveCooldownSeconds, 60.0f, 900.0f },
        };

        // Papyrus strings cross the VM boundary as BSFixedString. Skyrim's
        // global fixed-string table is case-insensitive and preserves whichever
        // spelling was interned first, so the key "enabled" commonly arrives as
        // "Enabled" after SkyUI has drawn that label. Setting keys are unique
        // ignoring ASCII case; compare them that way instead of trusting the
        // fixed string's presentation spelling.
        constexpr bool SameSettingName(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size()) {
                return false;
            }
            return std::ranges::equal(left, right, [](char a, char b) {
                const auto fold = [](char value) {
                    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
                };
                return fold(a) == fold(b);
            });
        }
        static_assert(SameSettingName("enabled", "Enabled"));

        template <class Field, std::size_t N>
        const Field* FindField(const Field (&fields)[N], std::string_view name)
        {
            const auto found = std::ranges::find_if(fields, [&](const Field& field) {
                return SameSettingName(field.name, name);
            });
            return found == std::end(fields) ? nullptr : std::addressof(*found);
        }

        std::string_view Text(RE::BSFixedString value)
        {
            const auto* text = value.c_str();
            return text ? std::string_view{ text } : std::string_view{};
        }

        void Unknown(std::string_view type, std::string_view name)
        {
            logger::error("MCM: unknown {} setting '{}'", type, name);
        }

        Lens* LensAt(Settings& settings, std::int32_t index)
        {
            if (index < 0 || index >= kMaxLenses) {
                logger::error("MCM: lens index {} is outside 0..{}", index, kMaxLenses - 1);
                return nullptr;
            }
            return std::addressof(settings.lenses[static_cast<std::size_t>(index)]);
        }

        const Lens* LensAt(const Settings& settings, std::int32_t index)
        {
            if (index < 0 || index >= kMaxLenses) {
                logger::error("MCM: lens index {} is outside 0..{}", index, kMaxLenses - 1);
                return nullptr;
            }
            return std::addressof(settings.lenses[static_cast<std::size_t>(index)]);
        }

        template <std::size_t N>
        bool AssignBuffer(char (&destination)[N], std::string_view value)
        {
            const auto copied = std::min(value.size(), N - 1);
            std::memcpy(destination, value.data(), copied);
            destination[copied] = '\0';
            return copied == value.size();
        }

        struct TextRow
        {
            std::string label;
            std::string value;
        };

        struct PendingViewRow
        {
            PendingImpulses::EntryId id = 0;
            std::string label;
            std::string value;
            std::string details;
        };

        struct DetailRow
        {
            std::string label;
            std::string value;
            std::string details;
        };

        // A Papyrus page is built through several native calls. Freeze each
        // page once at OnPageReset so row indexes still identify the same entry
        // if the Director changes live state before the player selects one.
        std::mutex                  g_viewLock;
        std::string                 g_statusSummary;
        std::vector<TextRow>        g_statusRows;
        std::string                 g_pendingSummary;
        std::vector<PendingViewRow> g_pendingRows;
        std::string                 g_ledgerSummary;
        std::vector<DetailRow>      g_ledgerRows;
        std::string                 g_historySummary;
        std::vector<DetailRow>      g_historyRows;
        std::string                 g_lastContext;

        bool ValidViewIndex(std::int32_t index, std::size_t size)
        {
            return index >= 0 && static_cast<std::size_t>(index) < size;
        }

        RE::BSFixedString Fixed(const std::string& value)
        {
            return RE::BSFixedString{ value.c_str() };
        }

        std::string RealDuration(double gameMinutes, float timescale)
        {
            gameMinutes = std::max(0.0, gameMinutes);
            if (timescale <= 0.0f) {
                return "time is frozen";
            }
            const auto realMinutes = gameMinutes / timescale;
            if (realMinutes < 1.0) {
                return std::format("{:.0f} real sec", realMinutes * 60.0);
            }
            if (realMinutes < 90.0) {
                return std::format("{:.0f} real min", realMinutes);
            }
            return std::format("{:.1f} real h", realMinutes / 60.0);
        }

        void RefreshStatusView(RE::StaticFunctionTag*)
        {
            Status state;
            WithState([&](Status& live) { state = live; });
            const auto settings = SnapshotSettings();

            std::string summary;
            if (!settings.enabled) {
                summary = "Disabled - no impulses will fire.";
            } else if (!state.skyrimNetAvailable) {
                summary = "SkyrimNet not found - nothing can run.";
            } else if (!state.snapshot.valid) {
                summary = "Waiting for a loaded save.";
            } else if (state.inFlight) {
                summary = "Generating an impulse now.";
            } else if (state.deliveryPending && state.cueSpacingRemainingSeconds > 0.0) {
                summary = std::format("Carried - a cue is waiting behind party-wide cue spacing ({:.0f} seconds "
                                      "remaining).",
                                      state.cueSpacingRemainingSeconds);
            } else if (state.deliveryPending && (state.inConversation || state.inVanillaDialogue)) {
                summary = std::format("Carried - a cue is held until the {} conversation is over.",
                                      state.inVanillaDialogue ? "current" : "party's");
            } else if (state.deliveryPending) {
                summary = std::format("Carried - a cue has waited {:.0f} of {:.0f} seconds for a quiet gap.",
                                      state.deliveryHeldSeconds, settings.maxDeferSeconds);
            } else if (!state.holdReason.empty()) {
                summary = std::format("Holding: {}", state.holdReason);
            } else if (state.lensClocks.empty()) {
                summary = "Running - lens clocks arm on the next pass.";
            } else {
                summary = "Running - every lens is counting down on its own clock.";
            }

            std::vector<TextRow> rows;
            rows.reserve(state.lensClocks.size() + state.snapshot.followers.size() + 16);
            for (const auto& clock : state.lensClocks) {
                if (clock.inFlight) {
                    rows.push_back({ clock.name, "asking now" });
                    continue;
                }
                const auto minutes = (clock.dueGameDays - state.snapshot.gameDays) * 24.0 * 60.0;
                rows.push_back({ clock.name,
                                 std::format("asks in {:.0f} game min ({})", std::max(0.0, minutes),
                                             RealDuration(minutes, state.snapshot.timescale)) });
            }

            if (state.snapshot.valid) {
                rows.push_back({ "Timescale",
                                 state.snapshot.timescale <= 0.0f
                                     ? "0 - the game clock is frozen"
                                     : std::format("{:.0f} game min per real min", state.snapshot.timescale) });
            }
            rows.push_back({ "SkyrimNet",
                             state.skyrimNetAvailable
                                 ? std::format("connected (API v{})", state.skyrimNetVersion)
                                 : "not found - SkyrimNet.dll did not load" });
            if (settings.deferOnConversation || settings.injectQuietGap) {
                rows.push_back({ "Conversation bridge",
                                 state.quiet.valid ? "reporting" : "silent - AgencyEngine_Bridge.pex may be stale" });
            }
            if (settings.combatContinuousMode && state.gameMasterOff) {
                rows.push_back({ "GameMaster",
                                 "disabled in SkyrimNet - continuous mode requests are ignored" });
            }
            if (!state.lastError.empty()) {
                rows.push_back({ "Last error", state.lastError });
            }

            if (!state.snapshot.valid) {
                rows.push_back({ "World", "No save loaded" });
            } else {
                rows.push_back({ "Player", state.snapshot.playerName });
                rows.push_back({ "Location",
                                 state.snapshot.location.empty() ? "(unknown)" : state.snapshot.location });
                rows.push_back({ "In combat", state.snapshot.playerInCombat ? "yes" : "no" });
                rows.push_back({ "Followers", std::to_string(state.snapshot.followers.size()) });
                for (const auto& follower : state.snapshot.followers) {
                    rows.push_back({ "Follower", std::format("{} ({:08X})", follower.name, follower.formID) });
                }
            }

            rows.push_back({ "This session",
                             std::format("{} carried, {} quiet", state.impulsesThisSession,
                                         state.silencesThisSession) });
            for (const auto& tally : state.lensTallies) {
                rows.push_back({ std::format("{} session", tally.name),
                                 std::format("{} carried, {} quiet", tally.carried, tally.quiet) });
            }
            rows.push_back({ "Carried, unsaid",
                             state.pendingImpulses.empty()
                                 ? "Nobody is carrying anything"
                                 : std::format("{} impulse(s) open", state.pendingImpulses.size()) });
            if (state.deliveryPending && state.cueSpacingRemainingSeconds > 0.0) {
                rows.push_back({ "Cue spacing",
                                 std::format("{:.0f} seconds remaining; oldest waiting companion is next",
                                             state.cueSpacingRemainingSeconds) });
            }

            if (settings.deferOnConversation || settings.injectQuietGap || state.inVanillaDialogue) {
                std::string conversation;
                if (state.inVanillaDialogue) {
                    conversation = "Player is in a Skyrim conversation";
                } else if (!state.quiet.valid) {
                    conversation = "No reading yet";
                } else if (state.quiet.recording) {
                    conversation = "Player is speaking into the microphone";
                } else if (state.quiet.speechQueue > 0) {
                    conversation = std::format("{} speech line(s) queued", state.quiet.speechQueue);
                } else if (state.inConversation) {
                    conversation = std::format("Quiet {:.1f}s, but the party conversation is still settling",
                                               state.quiet.msSinceAudioEnded / 1000.0);
                } else {
                    conversation = std::format("Quiet for {:.1f}s (threshold {:.0f}s)",
                                               state.quiet.msSinceAudioEnded / 1000.0, settings.quietSeconds);
                }
                rows.push_back({ "Conversation", std::move(conversation) });
            }
            if (settings.combatContinuousMode) {
                auto value = state.continuousEnabled ? "on"s : "off"s;
                if (state.continuousOwned) {
                    value += " (held by AgencyEngine)";
                }
                if (state.continuousPending) {
                    value += " - waiting for Papyrus";
                }
                rows.push_back({ "Continuous mode", std::move(value) });
            }

            std::scoped_lock lock{ g_viewLock };
            g_statusSummary = std::move(summary);
            g_statusRows = std::move(rows);
        }

        RE::BSFixedString GetStatusSummary(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return Fixed(g_statusSummary);
        }

        std::int32_t GetStatusRowCount(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return static_cast<std::int32_t>(g_statusRows.size());
        }

        RE::BSFixedString GetStatusRowLabel(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_statusRows.size()) ? Fixed(g_statusRows[static_cast<std::size_t>(index)].label)
                                                             : RE::BSFixedString{};
        }

        RE::BSFixedString GetStatusRowValue(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_statusRows.size()) ? Fixed(g_statusRows[static_cast<std::size_t>(index)].value)
                                                             : RE::BSFixedString{};
        }

        void RequestImpulseNow(RE::StaticFunctionTag*)
        {
            Director::RequestFireNow();
        }

        void RestartImpulseTimer(RE::StaticFunctionTag*)
        {
            Director::ResetTimer();
        }

        void RefreshPendingView(RE::StaticFunctionTag*)
        {
            const auto pending = PendingImpulses::Snapshot();
            GameSnapshot snapshot;
            std::vector<PendingImpulses::FloorGrant> floorOwners;
            std::size_t personalMemory = 0;
            std::size_t partyMemory = 0;
            std::size_t queuedEvidence = 0;
            std::size_t eligibleEntries = 0;
            bool batchInFlight = false;
            std::string lastTrigger;
            std::uint64_t callsAttempted = 0;
            std::uint64_t entriesClassified = 0;
            std::uint64_t zeroCallRaises = 0;
            std::uint64_t staleResults = 0;
            std::uint64_t queueOverflow = 0;
            std::uint64_t evidenceWatermark = 0;
            bool pollBaseline = false;
            double pollMilliseconds = 0.0;
            std::size_t pollTailEvents = 0;
            std::size_t pollRecoveredEvents = 0;
            std::uint64_t pollFailures = 0;
            WithState([&](Status& state) {
                snapshot = state.snapshot;
                floorOwners = state.floorOwners;
                personalMemory = state.personalMemoryRecords;
                partyMemory = state.partyMemoryRecords;
                queuedEvidence = state.resolutionQueuedEvidence;
                eligibleEntries = state.resolutionEligibleEntries;
                batchInFlight = state.resolutionBatchInFlight;
                lastTrigger = state.resolutionLastTrigger;
                callsAttempted = state.resolutionCallsAttempted;
                entriesClassified = state.resolutionEntriesClassified;
                zeroCallRaises = state.resolutionZeroCallRaises;
                staleResults = state.resolutionStaleResults;
                queueOverflow = state.resolutionQueueOverflow;
                evidenceWatermark = state.resolutionEvidenceWatermark;
                pollBaseline = state.resolutionPollBaseline;
                pollMilliseconds = state.resolutionPollLastMilliseconds;
                pollTailEvents = state.resolutionPollTailEvents;
                pollRecoveredEvents = state.resolutionPollRecoveredEvents;
                pollFailures = state.resolutionPollFailures;
            });
            const auto settings = SnapshotSettings();

            std::size_t carried = 0;
            std::size_t said = 0;
            std::vector<PendingViewRow> rows;
            rows.reserve(pending.size());
            for (const auto& entry : pending) {
                const bool raised = entry.state == PendingImpulses::LifecycleState::RaisedUnmet;
                (raised ? said : carried) += 1;
                const auto anchor = raised ? entry.raisedGameDays : entry.createdGameDays;
                const auto ageMinutes = (snapshot.gameDays - anchor) * 24.0 * 60.0;
                const auto ttlLeft = settings.pendingTtlGameMinutes - ageMinutes;
                const auto scale = snapshot.valid ? snapshot.timescale : 20.0f;

                std::string details = std::format("Entry #{} | {} -> {}\nState: {}\n", entry.id,
                                                  entry.speakerName, entry.target.name,
                                                  PendingImpulses::ToString(entry.state));
                details += entry.topic.empty() ? "Subject: none recorded\n"
                                               : std::format("Subject: {}\n", entry.topic);
                details += entry.text;
                details += std::format("\n\n{} for {:.0f} game min ({})", raised ? "Raised/unmet" : "Untouched",
                                       std::max(0.0, ageMinutes), RealDuration(ageMinutes, scale));
                if (settings.pendingTtlGameMinutes > 0.0f) {
                    details += std::format("\nRetires in {:.0f} game min ({})", std::max(0.0, ttlLeft),
                                           RealDuration(ttlLeft, scale));
                }
                details += std::format("\nEvidence watermark: {}\nFinal fallback: {}",
                                       entry.lastAttemptedEvidenceSequence,
                                       entry.fallbackConsumed ? "consumed" : "available");
                if (entry.unverified) {
                    details += "\nRestoring from the last session - not in her prompt yet";
                }
                if (std::ranges::any_of(floorOwners,
                                        [&](const PendingImpulses::FloorGrant& floor) {
                                            return floor.entryId == entry.id;
                                        })) {
                    details += "\nFloor owner: active";
                }

                rows.push_back({ entry.id,
                                 entry.lens.empty() ? std::format("{} | #{}", entry.speakerName, entry.id)
                                                    : std::format("{} | {} | #{}", entry.speakerName, entry.lens,
                                                                  entry.id),
                                 std::format("{} -> {} [{}]", entry.speakerName, entry.target.name,
                                             PendingImpulses::ToString(entry.state)),
                                 std::move(details) });
            }

            auto summary = pending.empty()
                               ? "Nothing is open right now."s
                               : std::format("{} carried, {} raised and unanswered", carried, said);
            summary += std::format(" Memory: {} personal, {} party. Resolver: {} evidence, {} eligible, {}, "
                                   "last {}, {} paid batch(es), {} classified, {} zero-call raise(s), "
                                   "{} stale, {} overflow, watermark {}. Recovery poll: {} | every 15 active s | "
                                   "last {:.0f} ms | tail {} | recovered {} | failures {}.",
                                   personalMemory, partyMemory, queuedEvidence, eligibleEntries,
                                   batchInFlight ? "in flight" : "idle",
                                   lastTrigger.empty() ? "none" : lastTrigger, callsAttempted,
                                   entriesClassified, zeroCallRaises, staleResults, queueOverflow,
                                   evidenceWatermark, pollBaseline ? "baseline ready" : "baseline pending",
                                   pollMilliseconds, pollTailEvents, pollRecoveredEvents, pollFailures);

            std::scoped_lock lock{ g_viewLock };
            g_pendingSummary = std::move(summary);
            g_pendingRows = std::move(rows);
        }
        RE::BSFixedString GetPendingSummary(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return Fixed(g_pendingSummary);
        }

        std::int32_t GetPendingCount(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return static_cast<std::int32_t>(g_pendingRows.size());
        }

        RE::BSFixedString GetPendingLabel(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_pendingRows.size())
                       ? Fixed(g_pendingRows[static_cast<std::size_t>(index)].label)
                       : RE::BSFixedString{};
        }

        RE::BSFixedString GetPendingValue(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_pendingRows.size())
                       ? Fixed(g_pendingRows[static_cast<std::size_t>(index)].value)
                       : RE::BSFixedString{};
        }

        RE::BSFixedString GetPendingDetails(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_pendingRows.size())
                       ? Fixed(g_pendingRows[static_cast<std::size_t>(index)].details)
                       : RE::BSFixedString{};
        }

        std::optional<PendingImpulses::EntryId> PendingIdentity(std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            if (!ValidViewIndex(index, g_pendingRows.size())) {
                return std::nullopt;
            }
            return g_pendingRows[static_cast<std::size_t>(index)].id;
        }

        bool CheckPending(RE::StaticFunctionTag*, std::int32_t index)
        {
            const auto identity = PendingIdentity(index);
            if (!identity) {
                return false;
            }
            Director::RequestResolveCheck(*identity);
            return true;
        }

        bool StopPending(RE::StaticFunctionTag*, std::int32_t index)
        {
            const auto identity = PendingIdentity(index);
            return identity && PendingImpulses::StopCarrying(*identity, "stopped from SkyUI MCM");
        }

        bool ForgetPending(RE::StaticFunctionTag*, std::int32_t index)
        {
            const auto identity = PendingIdentity(index);
            return identity && PendingImpulses::ForgetSubject(*identity, "forgotten from SkyUI MCM");
        }

        std::int32_t CheckAllPending(RE::StaticFunctionTag*)
        {
            std::vector<PendingImpulses::EntryId> identities;
            {
                std::scoped_lock lock{ g_viewLock };
                identities.reserve(g_pendingRows.size());
                for (const auto& row : g_pendingRows) {
                    identities.push_back(row.id);
                }
            }
            for (const auto id : identities) {
                Director::RequestResolveCheck(id);
            }
            return static_cast<std::int32_t>(identities.size());
        }

        std::int32_t ForgetAllPending(RE::StaticFunctionTag*)
        {
            return static_cast<std::int32_t>(
                PendingImpulses::ForgetAll("forgotten from SkyUI MCM (forget all)"));
        }

        std::int32_t GetPendingQueued(RE::StaticFunctionTag*)
        {
            return static_cast<std::int32_t>(Director::PendingResolveRequests());
        }

        void RefreshLedgerView(RE::StaticFunctionTag*)
        {
            const auto slots = PendingImpulses::LedgerSnapshot();
            const auto settings = SnapshotSettings();

            const auto ringOf = [&](std::string_view lens) -> std::string_view {
                for (const auto& configured : settings.lenses) {
                    if (configured.name[0] != '\0' && lens == configured.name) {
                        return lens;
                    }
                }
                return {};
            };
            const auto capFor = [&](std::string_view ring) {
                for (const auto& configured : settings.lenses) {
                    if (configured.name[0] != '\0' && ring == configured.name) {
                        return configured.ledgerSlots > 0 ? configured.ledgerSlots : settings.ledgerSlots;
                    }
                }
                return settings.ledgerSlots;
            };

            std::vector<DetailRow> rows;
            rows.reserve(slots.size());
            for (const auto& slot : slots) {
                const auto ring = ringOf(slot.lens);
                const auto held = std::ranges::count_if(slots, [&](const PendingImpulses::LedgerSlot& candidate) {
                    return candidate.formID == slot.formID && ringOf(candidate.lens) == ring;
                });
                const auto ringName = ring.empty() ? "shared"sv : ring;
                rows.push_back({
                    std::format("{} | {}", slot.speakerName, ringName),
                    slot.topic + (slot.provisional ? " (awaiting verdict)" : ""),
                    std::format("{}\n{} ring: {} of {}\nSubject: {}\n{}", slot.speakerName, ringName, held,
                                capFor(ring), slot.topic,
                                slot.provisional ? "Waiting to see whether it was answered"
                                                 : "Confirmed; suppressed until this ring evicts it"),
                });
            }

            std::scoped_lock lock{ g_viewLock };
            g_ledgerSummary = slots.empty()
                                  ? "Nothing yet. Subjects appear after a companion raises one."s
                                  : std::format("{} remembered subject(s)", slots.size());
            g_ledgerRows = std::move(rows);
        }

        RE::BSFixedString GetLedgerSummary(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return Fixed(g_ledgerSummary);
        }

        std::int32_t GetLedgerCount(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return static_cast<std::int32_t>(g_ledgerRows.size());
        }

        RE::BSFixedString GetLedgerLabel(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_ledgerRows.size())
                       ? Fixed(g_ledgerRows[static_cast<std::size_t>(index)].label)
                       : RE::BSFixedString{};
        }

        RE::BSFixedString GetLedgerValue(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_ledgerRows.size())
                       ? Fixed(g_ledgerRows[static_cast<std::size_t>(index)].value)
                       : RE::BSFixedString{};
        }

        RE::BSFixedString GetLedgerDetails(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_ledgerRows.size())
                       ? Fixed(g_ledgerRows[static_cast<std::size_t>(index)].details)
                       : RE::BSFixedString{};
        }

        void RefreshHistoryView(RE::StaticFunctionTag*)
        {
            std::deque<Impulse> history;
            std::string lastContext;
            WithState([&](Status& state) {
                history = state.history;
                lastContext = state.lastContextJson;
            });

            std::vector<DetailRow> rows;
            rows.reserve(history.size());
            for (const auto& impulse : history) {
                auto label = impulse.when;
                auto value = impulse.speaker.empty() ? "quiet"s
                                                     : std::format("{} -> {}", impulse.speaker, impulse.target);
                std::string details;
                if (!impulse.ok) {
                    details += "Delivery failed\n";
                }
                if (!impulse.speaker.empty()) {
                    details += std::format("{} -> {}\n", impulse.speaker, impulse.target);
                }
                if (!impulse.topic.empty()) {
                    details += std::format("Subject: {}\n", impulse.topic);
                }
                details += impulse.content;
                details += impulse.lens.empty()
                               ? std::format("\n\nVia {}", impulse.delivery)
                               : std::format("\n\nVia {} | {} question", impulse.delivery, impulse.lens);
                rows.push_back({ std::move(label), std::move(value), std::move(details) });
            }

            std::scoped_lock lock{ g_viewLock };
            g_historySummary = history.empty() ? "No impulses generated yet."s
                                               : std::format("{} recent ask(s)", history.size());
            g_historyRows = std::move(rows);
            g_lastContext = std::move(lastContext);
        }

        RE::BSFixedString GetHistorySummary(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return Fixed(g_historySummary);
        }

        std::int32_t GetHistoryCount(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return static_cast<std::int32_t>(g_historyRows.size());
        }

        RE::BSFixedString GetHistoryLabel(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_historyRows.size())
                       ? Fixed(g_historyRows[static_cast<std::size_t>(index)].label)
                       : RE::BSFixedString{};
        }

        RE::BSFixedString GetHistoryValue(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_historyRows.size())
                       ? Fixed(g_historyRows[static_cast<std::size_t>(index)].value)
                       : RE::BSFixedString{};
        }

        RE::BSFixedString GetHistoryDetails(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_viewLock };
            return ValidViewIndex(index, g_historyRows.size())
                       ? Fixed(g_historyRows[static_cast<std::size_t>(index)].details)
                       : RE::BSFixedString{};
        }

        RE::BSFixedString GetLastContext(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_viewLock };
            return Fixed(g_lastContext);
        }

        bool GetBool(RE::StaticFunctionTag*, RE::BSFixedString key)
        {
            const auto name = Text(key);
            const auto* field = FindField(kBoolFields, name);
            if (!field) {
                Unknown("boolean", name);
                return false;
            }

            std::scoped_lock lock{ g_settingsLock };
            return g_settings.*(field->member);
        }

        bool SetBool(RE::StaticFunctionTag*, RE::BSFixedString key, bool value)
        {
            const auto name = Text(key);
            const auto* field = FindField(kBoolFields, name);
            if (!field) {
                Unknown("boolean", name);
                return false;
            }

            std::scoped_lock lock{ g_settingsLock };
            g_settings.*(field->member) = value;
            return true;
        }

        std::int32_t GetInt(RE::StaticFunctionTag*, RE::BSFixedString key)
        {
            const auto name = Text(key);
            const auto* field = FindField(kIntFields, name);
            if (!field) {
                Unknown("integer", name);
                return 0;
            }

            std::scoped_lock lock{ g_settingsLock };
            return g_settings.*(field->member);
        }

        bool SetInt(RE::StaticFunctionTag*, RE::BSFixedString key, std::int32_t value)
        {
            const auto name = Text(key);
            const auto* field = FindField(kIntFields, name);
            if (!field) {
                Unknown("integer", name);
                return false;
            }

            std::scoped_lock lock{ g_settingsLock };
            g_settings.*(field->member) = std::clamp(value, field->minimum, field->maximum);
            return true;
        }

        float GetFloat(RE::StaticFunctionTag*, RE::BSFixedString key)
        {
            const auto name = Text(key);
            const auto* field = FindField(kFloatFields, name);
            if (!field) {
                Unknown("float", name);
                return 0.0f;
            }

            std::scoped_lock lock{ g_settingsLock };
            return g_settings.*(field->member);
        }

        bool SetFloat(RE::StaticFunctionTag*, RE::BSFixedString key, float value)
        {
            const auto name = Text(key);
            const auto* field = FindField(kFloatFields, name);
            if (!field) {
                Unknown("float", name);
                return false;
            }

            std::scoped_lock lock{ g_settingsLock };
            g_settings.*(field->member) = std::clamp(value, field->minimum, field->maximum);
            return true;
        }

        RE::BSFixedString GetString(RE::StaticFunctionTag*, RE::BSFixedString key)
        {
            const auto name = Text(key);
            std::scoped_lock lock{ g_settingsLock };
            if (SameSettingName(name, "eventTypeFilter"sv)) {
                return RE::BSFixedString{ g_settings.eventTypeFilter };
            }
            if (SameSettingName(name, "followerEventTypeFilter"sv)) {
                return RE::BSFixedString{ g_settings.followerEventTypeFilter };
            }

            Unknown("string", name);
            return {};
        }

        bool SetString(RE::StaticFunctionTag*, RE::BSFixedString key, RE::BSFixedString value)
        {
            const auto name = Text(key);
            const auto text = Text(value);
            std::scoped_lock lock{ g_settingsLock };
            if (SameSettingName(name, "eventTypeFilter"sv)) {
                return AssignBuffer(g_settings.eventTypeFilter, text);
            }
            if (SameSettingName(name, "followerEventTypeFilter"sv)) {
                return AssignBuffer(g_settings.followerEventTypeFilter, text);
            }

            Unknown("string", name);
            return false;
        }

        std::int32_t GetLensCount(RE::StaticFunctionTag*)
        {
            return kMaxLenses;
        }

        bool IsLensBuiltin(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens && BuiltinLensFor(lens->id) != nullptr;
        }

        RE::BSFixedString GetLensName(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens ? RE::BSFixedString{ lens->name } : RE::BSFixedString{};
        }

        bool SetLensName(RE::StaticFunctionTag*, std::int32_t index, RE::BSFixedString value)
        {
            std::scoped_lock lock{ g_settingsLock };
            auto* lens = LensAt(g_settings, index);
            if (!lens || BuiltinLensFor(lens->id)) {
                return false;
            }
            return AssignBuffer(lens->name, Text(value));
        }

        RE::BSFixedString GetLensPrompt(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens ? RE::BSFixedString{ lens->prompt } : RE::BSFixedString{};
        }

        bool SetLensPrompt(RE::StaticFunctionTag*, std::int32_t index, RE::BSFixedString value)
        {
            std::scoped_lock lock{ g_settingsLock };
            auto* lens = LensAt(g_settings, index);
            if (!lens || BuiltinLensFor(lens->id)) {
                return false;
            }

            const auto complete = AssignBuffer(lens->prompt, Text(value));
            if (lens->prompt[0] == '\0') {
                lens->enabled = false;
            }
            return complete;
        }

        bool GetLensEnabled(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens && lens->enabled;
        }

        bool SetLensEnabled(RE::StaticFunctionTag*, std::int32_t index, bool value)
        {
            std::scoped_lock lock{ g_settingsLock };
            auto* lens = LensAt(g_settings, index);
            if (!lens || (value && lens->prompt[0] == '\0')) {
                return false;
            }
            lens->enabled = value;
            return true;
        }

        float GetLensInterval(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens ? lens->intervalGameMinutes : 0.0f;
        }

        bool SetLensInterval(RE::StaticFunctionTag*, std::int32_t index, float value)
        {
            std::scoped_lock lock{ g_settingsLock };
            auto* lens = LensAt(g_settings, index);
            if (!lens) {
                return false;
            }
            lens->intervalGameMinutes = std::clamp(value, 15.0f, 2880.0f);
            return true;
        }

        float GetLensCooldown(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens ? lens->cooldownGameMinutes : 0.0f;
        }

        bool SetLensCooldown(RE::StaticFunctionTag*, std::int32_t index, float value)
        {
            std::scoped_lock lock{ g_settingsLock };
            auto* lens = LensAt(g_settings, index);
            if (!lens) {
                return false;
            }
            lens->cooldownGameMinutes = std::clamp(value, 0.0f, 5760.0f);
            return true;
        }

        bool GetLensProposal(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens && lens->proposal;
        }

        bool SetLensProposal(RE::StaticFunctionTag*, std::int32_t index, bool value)
        {
            std::scoped_lock lock{ g_settingsLock };
            auto* lens = LensAt(g_settings, index);
            if (!lens || BuiltinLensFor(lens->id)) {
                return false;
            }
            lens->proposal = value;
            return true;
        }

        std::int32_t GetLensSlots(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::scoped_lock lock{ g_settingsLock };
            const auto* lens = LensAt(g_settings, index);
            return lens ? lens->ledgerSlots : 0;
        }

        bool SetLensSlots(RE::StaticFunctionTag*, std::int32_t index, std::int32_t value)
        {
            std::scoped_lock lock{ g_settingsLock };
            auto* lens = LensAt(g_settings, index);
            if (!lens) {
                return false;
            }
            lens->ledgerSlots = std::clamp(value, 0, 20);
            return true;
        }

        bool RequestLens(RE::StaticFunctionTag*, std::int32_t index)
        {
            std::string key;
            {
                std::scoped_lock lock{ g_settingsLock };
                const auto* lens = LensAt(g_settings, index);
                if (!lens || !lens->enabled || lens->prompt[0] == '\0') {
                    return false;
                }
                key = lens->id[0] != '\0' ? lens->id : lens->prompt;
            }
            Director::RequestFireNow(key);
            return true;
        }

        void RestoreLensDefaults(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_settingsLock };
            for (auto& lens : g_settings.lenses) {
                const auto* builtin = BuiltinLensFor(lens.id);
                if (!builtin) {
                    continue;
                }
                lens.enabled = builtin->enabled;
                lens.intervalGameMinutes = builtin->intervalGameMinutes;
                lens.cooldownGameMinutes = builtin->cooldownGameMinutes;
                lens.ledgerSlots = builtin->ledgerSlots;
            }
        }

        void ResetSettings(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_settingsLock };
            g_settings = Settings{};
        }

        bool ReloadSettings(RE::StaticFunctionTag*)
        {
            std::scoped_lock lock{ g_settingsLock };
            return g_settings.Load();
        }

        bool SaveSettings(RE::StaticFunctionTag*)
        {
            return SnapshotSettings().Save();
        }
    }

    bool Register(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) {
            return false;
        }

        vm->RegisterFunction("RefreshStatusView", kScriptName, RefreshStatusView);
        vm->RegisterFunction("GetStatusSummary", kScriptName, GetStatusSummary);
        vm->RegisterFunction("GetStatusRowCount", kScriptName, GetStatusRowCount);
        vm->RegisterFunction("GetStatusRowLabel", kScriptName, GetStatusRowLabel);
        vm->RegisterFunction("GetStatusRowValue", kScriptName, GetStatusRowValue);
        vm->RegisterFunction("RequestImpulseNow", kScriptName, RequestImpulseNow);
        vm->RegisterFunction("RestartImpulseTimer", kScriptName, RestartImpulseTimer);
        vm->RegisterFunction("RefreshPendingView", kScriptName, RefreshPendingView);
        vm->RegisterFunction("GetPendingSummary", kScriptName, GetPendingSummary);
        vm->RegisterFunction("GetPendingCount", kScriptName, GetPendingCount);
        vm->RegisterFunction("GetPendingLabel", kScriptName, GetPendingLabel);
        vm->RegisterFunction("GetPendingValue", kScriptName, GetPendingValue);
        vm->RegisterFunction("GetPendingDetails", kScriptName, GetPendingDetails);
        vm->RegisterFunction("CheckPending", kScriptName, CheckPending);
        vm->RegisterFunction("StopPending", kScriptName, StopPending);
        vm->RegisterFunction("ForgetPending", kScriptName, ForgetPending);
        vm->RegisterFunction("CheckAllPending", kScriptName, CheckAllPending);
        vm->RegisterFunction("ForgetAllPending", kScriptName, ForgetAllPending);
        vm->RegisterFunction("GetPendingQueued", kScriptName, GetPendingQueued);
        vm->RegisterFunction("RefreshLedgerView", kScriptName, RefreshLedgerView);
        vm->RegisterFunction("GetLedgerSummary", kScriptName, GetLedgerSummary);
        vm->RegisterFunction("GetLedgerCount", kScriptName, GetLedgerCount);
        vm->RegisterFunction("GetLedgerLabel", kScriptName, GetLedgerLabel);
        vm->RegisterFunction("GetLedgerValue", kScriptName, GetLedgerValue);
        vm->RegisterFunction("GetLedgerDetails", kScriptName, GetLedgerDetails);
        vm->RegisterFunction("RefreshHistoryView", kScriptName, RefreshHistoryView);
        vm->RegisterFunction("GetHistorySummary", kScriptName, GetHistorySummary);
        vm->RegisterFunction("GetHistoryCount", kScriptName, GetHistoryCount);
        vm->RegisterFunction("GetHistoryLabel", kScriptName, GetHistoryLabel);
        vm->RegisterFunction("GetHistoryValue", kScriptName, GetHistoryValue);
        vm->RegisterFunction("GetHistoryDetails", kScriptName, GetHistoryDetails);
        vm->RegisterFunction("GetLastContext", kScriptName, GetLastContext);
        vm->RegisterFunction("GetBool", kScriptName, GetBool);
        vm->RegisterFunction("SetBool", kScriptName, SetBool);
        vm->RegisterFunction("GetInt", kScriptName, GetInt);
        vm->RegisterFunction("SetInt", kScriptName, SetInt);
        vm->RegisterFunction("GetFloat", kScriptName, GetFloat);
        vm->RegisterFunction("SetFloat", kScriptName, SetFloat);
        vm->RegisterFunction("GetString", kScriptName, GetString);
        vm->RegisterFunction("SetString", kScriptName, SetString);
        vm->RegisterFunction("GetLensCount", kScriptName, GetLensCount);
        vm->RegisterFunction("IsLensBuiltin", kScriptName, IsLensBuiltin);
        vm->RegisterFunction("GetLensName", kScriptName, GetLensName);
        vm->RegisterFunction("SetLensName", kScriptName, SetLensName);
        vm->RegisterFunction("GetLensPrompt", kScriptName, GetLensPrompt);
        vm->RegisterFunction("SetLensPrompt", kScriptName, SetLensPrompt);
        vm->RegisterFunction("GetLensEnabled", kScriptName, GetLensEnabled);
        vm->RegisterFunction("SetLensEnabled", kScriptName, SetLensEnabled);
        vm->RegisterFunction("GetLensInterval", kScriptName, GetLensInterval);
        vm->RegisterFunction("SetLensInterval", kScriptName, SetLensInterval);
        vm->RegisterFunction("GetLensCooldown", kScriptName, GetLensCooldown);
        vm->RegisterFunction("SetLensCooldown", kScriptName, SetLensCooldown);
        vm->RegisterFunction("GetLensProposal", kScriptName, GetLensProposal);
        vm->RegisterFunction("SetLensProposal", kScriptName, SetLensProposal);
        vm->RegisterFunction("GetLensSlots", kScriptName, GetLensSlots);
        vm->RegisterFunction("SetLensSlots", kScriptName, SetLensSlots);
        vm->RegisterFunction("RequestLens", kScriptName, RequestLens);
        vm->RegisterFunction("RestoreLensDefaults", kScriptName, RestoreLensDefaults);
        vm->RegisterFunction("ResetSettings", kScriptName, ResetSettings);
        vm->RegisterFunction("ReloadSettings", kScriptName, ReloadSettings);
        vm->RegisterFunction("SaveSettings", kScriptName, SaveSettings);

        logger::info("MCM: Papyrus settings functions registered");
        return true;
    }
}
