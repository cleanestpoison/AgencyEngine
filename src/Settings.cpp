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

        using LensTable = std::array<Lens, kMaxLenses>;

        Lens* FindById(LensTable& table, std::string_view id)
        {
            if (id.empty()) {
                return nullptr;
            }
            for (auto& lens : table) {
                if (id == lens.id) {
                    return &lens;
                }
            }
            return nullptr;
        }

        // The first slot holding no lens at all, for a hand-authored row.
        Lens* FirstFreeSlot(LensTable& table)
        {
            for (auto& lens : table) {
                if (lens.id[0] == '\0' && lens.name[0] == '\0' && lens.prompt[0] == '\0') {
                    return &lens;
                }
            }
            return nullptr;
        }

        // Only the two fields that are a preference. A lens's name, prompt file
        // and proposal semantics come from the build every time — they describe
        // a file in the archive, and a config that could contradict them would
        // only ever be wrong about it.
        void ApplyOverride(Lens& lens, const nlohmann::json& entry)
        {
            if (entry.contains("weight")) {
                lens.weight = std::max(entry.value("weight", lens.weight), 0);
            }
            if (entry.contains("ledgerSlots")) {
                lens.ledgerSlots = std::max(entry.value("ledgerSlots", lens.ledgerSlots), 0);
            }
        }

        void ApplyLensOverrides(LensTable& table, const nlohmann::json& overrides)
        {
            for (const auto& [id, entry] : overrides.items()) {
                if (!entry.is_object()) {
                    continue;
                }
                auto* lens = FindById(table, id);
                if (!lens) {
                    // Either a lens this build has retired or one from a version
                    // ahead of it. Kept in neither case — but said out loud,
                    // because the alternative is a weight someone set having
                    // quietly no effect.
                    logger::warn("Settings: no lens called '{}' in this version — its settings were ignored", id);
                    continue;
                }
                ApplyOverride(*lens, entry);
            }
        }

        // Files written before the roster moved into the build hold the whole
        // roster as an array, so the shipped rows have to be recognised in it and
        // the rest carried over as the user's own.
        //
        // Matched on the prompt file rather than the name: the name is the field
        // the settings page always let people edit, and a renamed row is still
        // the shipped lens. The rename itself is dropped, since the name is now
        // content — which does orphan that row's ledger ring, so it is logged.
        void MigrateLegacyLenses(LensTable& table, const nlohmann::json& array)
        {
            std::vector<std::string> matched;
            std::vector<std::string> custom;
            std::vector<std::string> renamed;

            for (const auto& entry : array) {
                if (!entry.is_object()) {
                    continue;
                }
                const auto prompt = entry.value("prompt", std::string{});
                if (prompt.empty()) {
                    continue;
                }

                Lens* shipped = nullptr;
                for (auto& lens : table) {
                    if (lens.id[0] != '\0' && prompt == lens.prompt) {
                        shipped = &lens;
                        break;
                    }
                }

                if (shipped) {
                    const auto name = entry.value("name", std::string{});
                    if (!name.empty() && name != shipped->name) {
                        renamed.push_back(std::format("{} (was '{}')", shipped->name, name));
                    }
                    ApplyOverride(*shipped, entry);
                    matched.emplace_back(shipped->id);
                    continue;
                }

                auto* slot = FirstFreeSlot(table);
                if (!slot) {
                    logger::warn("Settings: no room left for the lens '{}' — it was dropped", prompt);
                    continue;
                }
                AssignBuffer(slot->name, entry.value("name", std::string{}));
                AssignBuffer(slot->prompt, prompt);
                slot->weight = std::max(entry.value("weight", 0), 0);
                slot->proposal = entry.value("proposal", false);
                slot->ledgerSlots = std::max(entry.value("ledgerSlots", 0), 0);
                custom.push_back(prompt);
            }

            // A shipped lens the old file didn't list is one the user deleted,
            // and deleting was how the old page said "never ask this". Weight 0
            // is how the new one says it, so that is what the intent becomes —
            // rather than the lens reappearing at its shipped weight, which is
            // the one outcome nobody who deleted a row is expecting.
            for (auto& lens : table) {
                if (lens.id[0] != '\0' && std::ranges::find(matched, lens.id) == matched.end()) {
                    lens.weight = 0;
                    logger::info("Settings: the {} lens was not in the old config, so it stays switched off "
                                 "(weight 0). Raise it on the Lenses tab if you want it.",
                                 lens.name);
                }
            }

            logger::info("Settings: migrated {} lens row(s) from the old format — {} shipped, {} your own. The "
                         "roster now comes from the mod itself, so a lens added in a later version turns up on "
                         "its own; the file keeps only what you changed.",
                         matched.size() + custom.size(), matched.size(), custom.size());
            for (const auto& note : renamed) {
                logger::info("Settings: the {} lens no longer carries your name for it. Anything it already "
                             "recorded in the ledger stays under the old name, in the shared list.",
                             note);
            }
        }

        void LoadCustomLenses(LensTable& table, const nlohmann::json& array)
        {
            for (const auto& entry : array) {
                if (!entry.is_object()) {
                    continue;
                }
                const auto prompt = entry.value("prompt", std::string{});
                if (prompt.empty()) {
                    continue;
                }
                auto* slot = FirstFreeSlot(table);
                if (!slot) {
                    logger::warn("Settings: no room left for the lens '{}' — it was dropped. {} lenses is the "
                                 "limit, of which {} are the mod's own.",
                                 prompt, kMaxLenses, kBuiltinLensCount);
                    return;
                }
                AssignBuffer(slot->name, entry.value("name", std::string{}));
                AssignBuffer(slot->prompt, prompt);
                slot->weight = std::max(entry.value("weight", 0), 0);
                slot->proposal = entry.value("proposal", false);
                slot->ledgerSlots = std::max(entry.value("ledgerSlots", 0), 0);
            }
        }
    }

    std::string Settings::LensSummary() const
    {
        std::string out;
        std::string off;
        for (const auto& lens : lenses) {
            if (lens.prompt[0] == '\0') {
                continue;
            }
            // Weight 0 is now the only way a lens is switched off — the roster
            // comes from the build, so there is no longer such a thing as a lens
            // that isn't there. Named rather than omitted: "the Activity lens is
            // installed and set to 0" and "this install predates the Activity
            // lens" are different problems with the same symptom.
            if (lens.weight <= 0) {
                off += off.empty() ? lens.name : std::format(", {}", lens.name);
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
        if (out.empty()) {
            return std::format("none usable — every lens is weighted 0 ({})", off);
        }
        return off.empty() ? out : std::format("{}; off: {}", out, off);
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

            // The roster is always the one this build ships; the file only ever
            // adjusts it. Reset first so a second Load can't accumulate.
            lenses = DefaultLenses();
            if (j.contains("lenses")) {
                if (j["lenses"].is_object()) {
                    ApplyLensOverrides(lenses, j["lenses"]);
                } else if (j["lenses"].is_array()) {
                    MigrateLegacyLenses(lenses, j["lenses"]);
                }
            }
            if (j.contains("customLenses") && j["customLenses"].is_array()) {
                LoadCustomLenses(lenses, j["customLenses"]);
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

            // Only what differs from what this build ships.
            //
            // The alternative — writing every key, as this used to — freezes
            // every default the moment anyone saves anything. From then on a
            // release that retunes a number reaches new installs and nobody
            // else, and the file gives no way to tell a value someone chose from
            // one that was merely current when they last pressed Save. So the
            // file records decisions, and everything else stays live.
            // Not `static`: MSVC constant-initializes a static const aggregate
            // to zeros and skips every default member initializer, so `shipped`
            // would hold an empty followerEventTypeFilter and a roster of blank
            // lenses — and the symptom is a file that writes back the fields
            // whose default is not zero, which is exactly the pinning this is
            // here to stop. A fresh one per save costs nothing; saves are rare.
            const Settings shipped{};
            nlohmann::json j = nlohmann::json::object();
            const auto     put = [&j](const char* key, const auto& value, const auto& fallback) {
                if (value != fallback) {
                    j[key] = value;
                }
            };

            put("enabled", enabled, shipped.enabled);
            put("intervalGameMinutes", intervalGameMinutes, shipped.intervalGameMinutes);
            put("maxEvents", maxEvents, shipped.maxEvents);
            put("delivery", delivery, shipped.delivery);
            put("generateThought", generateThought, shipped.generateThought);
            put("requireFollower", requireFollower, shipped.requireFollower);
            put("skipInCombat", skipInCombat, shipped.skipInCombat);
            put("perFollowerEvents", perFollowerEvents, shipped.perFollowerEvents);
            put("forcedImpulseChance", forcedImpulseChance, shipped.forcedImpulseChance);
            put("combatContinuousMode", combatContinuousMode, shipped.combatContinuousMode);
            put("continuousExitGraceSeconds", continuousExitGraceSeconds, shipped.continuousExitGraceSeconds);
            put("debugLog", debugLog, shipped.debugLog);
            put("deferOnConversation", deferOnConversation, shipped.deferOnConversation);
            put("quietSeconds", quietSeconds, shipped.quietSeconds);
            put("maxDeferSeconds", maxDeferSeconds, shipped.maxDeferSeconds);
            put("degradeToPersistentEvent", degradeToPersistentEvent, shipped.degradeToPersistentEvent);
            put("injectQuietGap", injectQuietGap, shipped.injectQuietGap);
            put("quietPollSeconds", quietPollSeconds, shipped.quietPollSeconds);
            put("pendingBioInjection", pendingBioInjection, shipped.pendingBioInjection);
            put("pendingTtlGameMinutes", pendingTtlGameMinutes, shipped.pendingTtlGameMinutes);
            put("pendingResolveGameMinutes", pendingResolveGameMinutes, shipped.pendingResolveGameMinutes);
            put("ledgerEnabled", ledgerEnabled, shipped.ledgerEnabled);
            put("ledgerSlots", ledgerSlots, shipped.ledgerSlots);
            put("ledgerVeto", ledgerVeto, shipped.ledgerVeto);
            put("eventTypeFilter", std::string{ eventTypeFilter }, std::string{ shipped.eventTypeFilter });
            put("followerEventTypeFilter", std::string{ followerEventTypeFilter },
                std::string{ shipped.followerEventTypeFilter });

            // Shipped lenses: the two tunable fields, and only where they were
            // moved. Everything else about a lens is the prompt file's business.
            nlohmann::json lensOverrides = nlohmann::json::object();
            for (const auto& lens : lenses) {
                const auto* builtin = BuiltinLensFor(lens.id);
                if (!builtin) {
                    continue;
                }
                nlohmann::json entry = nlohmann::json::object();
                if (lens.weight != builtin->weight) {
                    entry["weight"] = lens.weight;
                }
                if (lens.ledgerSlots != builtin->ledgerSlots) {
                    entry["ledgerSlots"] = lens.ledgerSlots;
                }
                if (!entry.empty()) {
                    lensOverrides[lens.id] = std::move(entry);
                }
            }
            if (!lensOverrides.empty()) {
                j["lenses"] = std::move(lensOverrides);
            }

            // A lens someone wrote themselves has no shipped row behind it, so
            // it is stored whole — this is the one place the file still owns a
            // lens rather than adjusting one.
            nlohmann::json customLenses = nlohmann::json::array();
            for (const auto& lens : lenses) {
                if (lens.id[0] != '\0' || lens.prompt[0] == '\0') {
                    continue;
                }
                customLenses.push_back(nlohmann::json{
                    { "name", std::string{ lens.name } },
                    { "prompt", std::string{ lens.prompt } },
                    { "weight", lens.weight },
                    { "proposal", lens.proposal },
                    { "ledgerSlots", lens.ledgerSlots },
                });
            }
            if (!customLenses.empty()) {
                j["customLenses"] = std::move(customLenses);
            }

            std::ofstream file{ path, std::ios::trunc };
            file << j.dump(2);
            // The count is worth saying: an empty file is the expected shape for
            // an untouched install, not a failed write, and it is what keeps a
            // retuned default reaching this install on the next update.
            logger::info("Settings: saved to {} — {} setting(s) differ from the mod's own defaults", path.string(),
                         j.size());
            logger::info("Settings: {}", Summary());
            return true;
        } catch (const std::exception& e) {
            logger::error("Settings: failed to write {}: {}", path.string(), e.what());
            return false;
        }
    }
}
