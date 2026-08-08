#include "Settings.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace AgencyEngine
{
    namespace
    {
        // Copy a std::string into a fixed char buffer, always NUL-terminated.
        template <std::size_t N>
        void AssignBuffer(char (&dst)[N], const std::string& src)
        {
            const auto n = std::min(src.size(), N - 1);
            std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        }
    }

    std::string Settings::LensSummary() const
    {
        std::string out;
        for (const auto& lens : lenses) {
            if (lens.weight <= 0 || lens.prompt[0] == '\0') {
                continue;
            }
            if (!out.empty()) {
                out += ", ";
            }
            // Every per-lens field, not just the weight: a log someone sends in
            // has to describe the configuration it was produced under, and
            // "proposals, three slots" is the difference between a lens that
            // recurs and one that vetoes itself into silence.
            out += std::format("{}({})={}{}{}", lens.name, lens.prompt, lens.weight,
                               lens.proposal ? " proposals" : "",
                               lens.ledgerSlots > 0 ? std::format(" slots={}", lens.ledgerSlots) : "");
        }
        // Worth spelling out rather than logging an empty list: every lens
        // weighted to zero is a valid configuration that stops the loop
        // entirely, and that is exactly the sort of thing someone reads their
        // own log to discover.
        return out.empty() ? std::string{ "none usable — every lens is weighted 0" } : out;
    }

    std::string Settings::Summary() const
    {
        return std::format(
            "enabled={} interval={:.0f} in-game min delivery={} generateThought={} requireFollower={} "
            "skipInCombat={} playerEvents={} perFollowerEvents={} forcedImpulseChance={}% eventFilter='{}' "
            "lenses=[{}] "
            "deferOnConversation={} quiet={:.0f}s maxDefer={:.0f}s onExpiry={} injectQuietGap={} poll={:.1f}s "
            "verboseLog={} combatContinuousMode={} continuousExitGrace={:.0f}s pendingBioInjection={} "
            "pendingTtl={:.0f} in-game min pendingResolve={:.0f} in-game min "
            "followerEventFilter='{}' ledger={} slots={} veto={}",
            enabled, intervalGameMinutes,
            delivery == kDirectNarration ? "direct-narration" : "persistent-event", generateThought,
            requireFollower, skipInCombat, maxEvents, perFollowerEvents, forcedImpulseChance, eventTypeFilter,
            LensSummary(),
            deferOnConversation, quietSeconds, maxDeferSeconds,
            degradeToPersistentEvent ? "persistent-event" : "drop", injectQuietGap, quietPollSeconds, debugLog,
            combatContinuousMode, continuousExitGraceSeconds, pendingBioInjection, pendingTtlGameMinutes,
            pendingResolveGameMinutes, followerEventTypeFilter, ledgerEnabled, ledgerSlots, ledgerVeto);
    }

    std::filesystem::path Settings::FilePath()
    {
        // Relative to the game's working directory, which is the SkyrimSE.exe
        // folder — so this resolves through the mod manager's VFS the same way
        // the DLL itself does, and writes land in MO2's overwrite.
        return std::filesystem::path{ "Data/SKSE/Plugins/AgencyEngine.json" };
    }

    bool Settings::Load()
    {
        const auto path = FilePath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            logger::info("Settings: no {} yet, using defaults", path.string());
            return false;
        }

        try {
            std::ifstream file{ path };
            nlohmann::json j;
            file >> j;

            enabled = j.value("enabled", enabled);
            intervalGameMinutes = j.value("intervalGameMinutes", intervalGameMinutes);
            maxEvents = j.value("maxEvents", maxEvents);
            delivery = j.value("delivery", delivery);
            generateThought = j.value("generateThought", generateThought);
            requireFollower = j.value("requireFollower", requireFollower);
            skipInCombat = j.value("skipInCombat", skipInCombat);
            perFollowerEvents = j.value("perFollowerEvents", perFollowerEvents);
            // Clamped on the way in: the template compares this against a
            // 0-100 roll, so an out-of-range value read from a hand-edited file
            // would silently mean "never" or "always" rather than what it says.
            forcedImpulseChance = std::clamp(j.value("forcedImpulseChance", forcedImpulseChance), 0, 100);
            combatContinuousMode = j.value("combatContinuousMode", combatContinuousMode);
            continuousExitGraceSeconds = j.value("continuousExitGraceSeconds", continuousExitGraceSeconds);
            debugLog = j.value("debugLog", debugLog);
            deferOnConversation = j.value("deferOnConversation", deferOnConversation);
            quietSeconds = j.value("quietSeconds", quietSeconds);
            maxDeferSeconds = j.value("maxDeferSeconds", maxDeferSeconds);
            degradeToPersistentEvent = j.value("degradeToPersistentEvent", degradeToPersistentEvent);
            injectQuietGap = j.value("injectQuietGap", injectQuietGap);
            quietPollSeconds = j.value("quietPollSeconds", quietPollSeconds);
            pendingBioInjection = j.value("pendingBioInjection", pendingBioInjection);
            pendingTtlGameMinutes = j.value("pendingTtlGameMinutes", pendingTtlGameMinutes);
            pendingResolveGameMinutes = j.value("pendingResolveGameMinutes", pendingResolveGameMinutes);
            ledgerEnabled = j.value("ledgerEnabled", ledgerEnabled);
            ledgerSlots = j.value("ledgerSlots", ledgerSlots);
            ledgerVeto = j.value("ledgerVeto", ledgerVeto);

            // A present "lenses" array replaces the defaults wholesale rather
            // than merging into them. Merging would make a deleted lens
            // reappear on the next load, which is the one behaviour nobody
            // expects from an editable list.
            if (j.contains("lenses") && j["lenses"].is_array()) {
                for (auto& lens : lenses) {
                    lens = Lens{};
                }
                int index = 0;
                for (const auto& entry : j["lenses"]) {
                    if (index >= kMaxLenses) {
                        logger::warn("Settings: {} lenses configured but only {} are supported — ignoring the rest",
                                     j["lenses"].size(), kMaxLenses);
                        break;
                    }
                    if (!entry.is_object()) {
                        continue;
                    }
                    AssignBuffer(lenses[index].name, entry.value("name", std::string{}));
                    AssignBuffer(lenses[index].prompt, entry.value("prompt", std::string{}));
                    lenses[index].weight = entry.value("weight", 0);
                    // Both absent from any file written before the Activity
                    // lens, and both default to what those installs already do:
                    // topics, on the global ledger count. An upgrade is not a
                    // reset — nobody's tuning changes because a field appeared.
                    lenses[index].proposal = entry.value("proposal", false);
                    lenses[index].ledgerSlots = std::max(entry.value("ledgerSlots", 0), 0);
                    ++index;
                }

                // Said out loud, because the consequence is invisible otherwise:
                // an upgrade that ships a new lens does not add it to a file
                // that already has a lens list, and the symptom is a lens the
                // release notes describe never firing. Wholesale replacement is
                // the deliberate part — a lens someone deleted must not come
                // back on the next load — so the fix is a line here and a row
                // added by hand, not a merge.
                logger::info("Settings: {} lens row(s) came from the file and replaced the shipped defaults. A lens "
                             "added in a later version does not appear on its own — add its row on the Lenses tab.",
                             index);
            }

            AssignBuffer(eventTypeFilter, j.value("eventTypeFilter", std::string{ eventTypeFilter }));
            AssignBuffer(followerEventTypeFilter,
                         j.value("followerEventTypeFilter", std::string{ followerEventTypeFilter }));

            logger::info("Settings: loaded from {}", path.string());
            logger::info("Settings: {}", Summary());
            return true;
        } catch (const std::exception& e) {
            logger::error("Settings: failed to read {}: {}", path.string(), e.what());
            return false;
        }
    }

    bool Settings::Save() const
    {
        const auto path = FilePath();
        try {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            // Empty-named, empty-prompted slots are UI padding, not
            // configuration — writing them back would grow the file with rows
            // that mean nothing.
            nlohmann::json lensArray = nlohmann::json::array();
            for (const auto& lens : lenses) {
                if (lens.name[0] == '\0' && lens.prompt[0] == '\0') {
                    continue;
                }
                lensArray.push_back(nlohmann::json{
                    { "name", std::string{ lens.name } },
                    { "prompt", std::string{ lens.prompt } },
                    { "weight", lens.weight },
                    { "proposal", lens.proposal },
                    { "ledgerSlots", lens.ledgerSlots },
                });
            }

            const nlohmann::json j{
                { "enabled", enabled },
                { "intervalGameMinutes", intervalGameMinutes },
                { "maxEvents", maxEvents },
                { "delivery", delivery },
                { "generateThought", generateThought },
                { "requireFollower", requireFollower },
                { "skipInCombat", skipInCombat },
                { "perFollowerEvents", perFollowerEvents },
                { "forcedImpulseChance", forcedImpulseChance },
                { "combatContinuousMode", combatContinuousMode },
                { "continuousExitGraceSeconds", continuousExitGraceSeconds },
                { "debugLog", debugLog },
                { "deferOnConversation", deferOnConversation },
                { "quietSeconds", quietSeconds },
                { "maxDeferSeconds", maxDeferSeconds },
                { "degradeToPersistentEvent", degradeToPersistentEvent },
                { "injectQuietGap", injectQuietGap },
                { "quietPollSeconds", quietPollSeconds },
                { "pendingBioInjection", pendingBioInjection },
                { "pendingTtlGameMinutes", pendingTtlGameMinutes },
                { "pendingResolveGameMinutes", pendingResolveGameMinutes },
                { "ledgerEnabled", ledgerEnabled },
                { "ledgerSlots", ledgerSlots },
                { "ledgerVeto", ledgerVeto },
                { "lenses", std::move(lensArray) },
                { "eventTypeFilter", std::string{ eventTypeFilter } },
                { "followerEventTypeFilter", std::string{ followerEventTypeFilter } },
            };

            std::ofstream file{ path, std::ios::trunc };
            file << j.dump(2);
            logger::info("Settings: saved to {}", path.string());
            logger::info("Settings: {}", Summary());
            return true;
        } catch (const std::exception& e) {
            logger::error("Settings: failed to write {}: {}", path.string(), e.what());
            return false;
        }
    }
}
