#include "PendingImpulse.h"

#include "Logging.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <ranges>

namespace AgencyEngine::PendingImpulses
{
    namespace
    {
        constexpr std::uint32_t kFormatVersion = 2;
        constexpr std::int64_t kWriteIntervalMs = 15000;
        constexpr std::size_t kMaxSaves = 20;

        std::mutex g_lock;
        std::vector<Entry> g_entries;
        std::vector<LedgerSlot> g_ledger;
        std::vector<PartyTopic> g_party;
        EntryId g_nextEntryId = 1;
        EvidenceSequence g_nextEvidenceSequence = 1;
        std::string g_loadedSaveId;
        bool g_dirty = false;
        bool g_migrated = false;
        std::int64_t g_lastWriteMs = 0;

        std::mutex g_floorLock;
        std::vector<FloorGrant> g_floor;

        std::atomic<std::size_t> g_cap{ 6 };
        std::atomic<float> g_partyEchoGameDays{ 7.0f };
        std::mutex g_ringLock;
        std::vector<LensRing> g_rings;

        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        std::vector<LensRing> RingsSnapshot()
        {
            std::scoped_lock lock{ g_ringLock };
            return g_rings;
        }

        std::string RingOf(std::string_view lens, const std::vector<LensRing>& rings)
        {
            if (!lens.empty() && std::ranges::any_of(rings, [&](const LensRing& ring) { return ring.name == lens; })) {
                return std::string{ lens };
            }
            return {};
        }

        std::size_t ResolveCap(std::string_view lens, std::size_t explicitCap)
        {
            if (explicitCap > 0) {
                return explicitCap;
            }
            if (!lens.empty()) {
                std::scoped_lock lock{ g_ringLock };
                for (const auto& ring : g_rings) {
                    if (ring.name == lens && ring.slots > 0) {
                        return ring.slots;
                    }
                }
            }
            return std::max<std::size_t>(g_cap.load(), 1);
        }

        bool CompatibleTargets(const TargetScope& left, const TargetScope& right)
        {
            return left.id == 0 || right.id == 0 || left.id == right.id;
        }

        Entry* FindLocked(EntryId id)
        {
            const auto it = std::ranges::find(g_entries, id, &Entry::id);
            return it == g_entries.end() ? nullptr : &*it;
        }

        void RemoveOwnedProvisionalLocked(EntryId id)
        {
            std::erase_if(g_ledger, [&](const LedgerSlot& slot) {
                return slot.originEntryId == id && slot.provisional;
            });
        }

        void RemoveOwnedMemoryLocked(EntryId id)
        {
            std::erase_if(g_ledger, [&](const LedgerSlot& slot) { return slot.originEntryId == id; });
            std::erase_if(g_party, [&](const PartyTopic& topic) { return topic.originEntryId == id; });
        }

        void RetireEntryLocked(std::vector<Entry>::iterator it, bool forgetMemory)
        {
            if (forgetMemory) {
                RemoveOwnedMemoryLocked(it->id);
            } else if (it->state == LifecycleState::Untouched) {
                RemoveOwnedProvisionalLocked(it->id);
            }
            g_entries.erase(it);
            g_dirty = true;
        }

        void TrimRingLocked(std::uint32_t formID, std::string_view lens, std::size_t cap,
                            const std::vector<LensRing>& rings)
        {
            const auto ring = RingOf(lens, rings);
            auto count = std::ranges::count_if(g_ledger, [&](const LedgerSlot& slot) {
                return slot.formID == formID && RingOf(slot.lens, rings) == ring;
            });
            while (count > static_cast<std::ptrdiff_t>(cap)) {
                const auto oldest = std::ranges::find_if(g_ledger, [&](const LedgerSlot& slot) {
                    return slot.formID == formID && RingOf(slot.lens, rings) == ring;
                });
                if (oldest == g_ledger.end()) {
                    break;
                }
                g_ledger.erase(oldest);
                --count;
            }
        }

        void RecordProvisionalLocked(const Entry& entry, std::size_t cap, const std::vector<LensRing>& rings)
        {
            if (entry.normalizedTopic.empty()) {
                return;
            }

            // Confirmation is monotonic and cross-lens. A new provisional carry
            // can neither move nor weaken an existing confirmed record.
            if (std::ranges::any_of(g_ledger, [&](const LedgerSlot& slot) {
                    return slot.formID == entry.formID && slot.normalizedTopic == entry.normalizedTopic &&
                           !slot.provisional;
                })) {
                return;
            }

            std::erase_if(g_ledger, [&](const LedgerSlot& slot) {
                return slot.formID == entry.formID && slot.normalizedTopic == entry.normalizedTopic &&
                       slot.provisional;
            });
            g_ledger.push_back({ entry.id, entry.formID, entry.speakerName, entry.topic, entry.normalizedTopic,
                                 entry.lens, true });
            TrimRingLocked(entry.formID, entry.lens, ResolveCap(entry.lens, cap), rings);
        }

        void ConfirmPersonalLocked(const Entry& entry)
        {
            for (auto& slot : g_ledger) {
                if (slot.originEntryId == entry.id) {
                    slot.provisional = false;
                    return;
                }
            }
            if (!entry.normalizedTopic.empty() &&
                !std::ranges::any_of(g_ledger, [&](const LedgerSlot& slot) {
                    return slot.formID == entry.formID && slot.normalizedTopic == entry.normalizedTopic &&
                           !slot.provisional;
                })) {
                const auto rings = RingsSnapshot();
                g_ledger.push_back({ entry.id, entry.formID, entry.speakerName, entry.topic, entry.normalizedTopic,
                                     entry.lens, false });
                TrimRingLocked(entry.formID, entry.lens, ResolveCap(entry.lens, 0), rings);
            }
        }

        void RecordPartyLocked(const Entry& entry, double raisedGameDays)
        {
            if (entry.normalizedTopic.empty()) {
                return;
            }
            std::erase_if(g_party, [&](const PartyTopic& topic) { return topic.originEntryId == entry.id; });
            g_party.push_back({ entry.id, entry.formID, entry.speakerName, entry.target, entry.topic,
                                entry.normalizedTopic, raisedGameDays });
            if (g_party.size() > kPartyLedgerCap) {
                g_party.erase(g_party.begin(), g_party.begin() + (g_party.size() - kPartyLedgerCap));
            }
        }

        void RetirePartyEchoesLocked(const Entry& origin)
        {
            const auto originId = origin.id;
            const auto originFormID = origin.formID;
            const auto normalizedTopic = origin.normalizedTopic;
            const auto target = origin.target;
            for (auto it = g_entries.begin(); it != g_entries.end();) {
                if (it->id != originId && it->formID != originFormID &&
                    it->state == LifecycleState::Untouched &&
                    it->normalizedTopic == normalizedTopic && CompatibleTargets(it->target, target)) {
                    RemoveOwnedProvisionalLocked(it->id);
                    it = g_entries.erase(it);
                    g_dirty = true;
                } else {
                    ++it;
                }
            }
        }

        bool RaiseLocked(Entry& entry, double raisedGameDays)
        {
            if (entry.state == LifecycleState::Met) {
                return false;
            }
            if (entry.state == LifecycleState::Untouched) {
                entry.state = LifecycleState::RaisedUnmet;
                entry.raisedGameDays = raisedGameDays;
            }
            ConfirmPersonalLocked(entry);
            RecordPartyLocked(entry, entry.raisedGameDays > 0.0 ? entry.raisedGameDays : raisedGameDays);
            RetirePartyEchoesLocked(entry);
            g_dirty = true;
            return true;
        }

        std::string RenderLocked(std::uint32_t formID, LifecycleState state)
        {
            std::string out;
            for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it) {
                if (it->formID != formID || it->unverified || it->state != state) {
                    continue;
                }
                out += out.empty() ? "- " : "\n- ";
                out += it->text;
            }
            return out;
        }

        double AgeAnchor(const Entry& entry)
        {
            return entry.state == LifecycleState::RaisedUnmet ? entry.raisedGameDays : entry.createdGameDays;
        }

        nlohmann::json TargetToJson(const TargetScope& target)
        {
            return { { "id", target.id }, { "name", target.name } };
        }

        TargetScope TargetFromJson(const nlohmann::json& value, std::string legacyName = {})
        {
            if (!value.is_object()) {
                return { 0, std::move(legacyName) };
            }
            return { value.value("id", 0ull), value.value("name", std::move(legacyName)) };
        }

        nlohmann::json EntryToJson(const Entry& entry)
        {
            return { { "id", entry.id },
                     { "formID", entry.formID },
                     { "speakerId", entry.speakerId },
                     { "speakerName", entry.speakerName },
                     { "target", TargetToJson(entry.target) },
                     { "text", entry.text },
                     { "topic", entry.topic },
                     { "normalizedTopic", entry.normalizedTopic },
                     { "lens", entry.lens },
                     { "proposal", entry.proposal },
                     { "state", ToString(entry.state) },
                     { "createdGameDays", entry.createdGameDays },
                     { "raisedGameDays", entry.raisedGameDays },
                     { "lastAttemptedEvidenceSequence", entry.lastAttemptedEvidenceSequence },
                     { "fallbackConsumed", entry.fallbackConsumed } };
        }

        Entry EntryFromJson(const nlohmann::json& value, bool legacy)
        {
            Entry entry;
            entry.id = legacy ? 0 : value.value("id", 0ull);
            entry.formID = value.value("formID", 0u);
            entry.speakerId = value.value("speakerId", 0ull);
            entry.speakerName = value.value("speakerName", std::string{});
            entry.target = TargetFromJson(value.value("target", nlohmann::json{}),
                                          value.value("targetName", std::string{}));
            entry.text = value.value("text", std::string{});
            entry.topic = value.value("topic", std::string{});
            entry.normalizedTopic = value.value("normalizedTopic", std::string{});
            if (entry.normalizedTopic.empty()) {
                if (const auto key = NormalizeTopic(entry.topic)) {
                    entry.normalizedTopic = *key;
                }
            }
            entry.lens = value.value("lens", std::string{});
            entry.proposal = value.value("proposal", false);
            entry.createdGameDays = value.value("createdGameDays", 0.0);
            if (legacy) {
                entry.state = value.value("spoken", false) ? LifecycleState::RaisedUnmet : LifecycleState::Untouched;
                entry.raisedGameDays = value.value("spokenGameDays", entry.createdGameDays);
            } else {
                entry.state = ParseLifecycleState(value.value("state", std::string{}))
                                  .value_or(LifecycleState::Untouched);
                entry.raisedGameDays = value.value("raisedGameDays", entry.createdGameDays);
                entry.lastAttemptedEvidenceSequence = value.value("lastAttemptedEvidenceSequence", 0ull);
                entry.fallbackConsumed = value.value("fallbackConsumed", false);
            }
            entry.unverified = true;
            return entry;
        }

        nlohmann::json ReadFile()
        {
            const auto path = FilePath();
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                return nlohmann::json::object();
            }
            try {
                std::ifstream file{ path };
                nlohmann::json root;
                file >> root;
                return root.is_object() ? root : nlohmann::json::object();
            } catch (const std::exception& error) {
                logger::warn("Pending impulses: could not read {} ({}) — starting empty", path.string(), error.what());
                return nlohmann::json::object();
            }
        }

        void BackupLegacySidecar()
        {
            const auto source = FilePath();
            auto backup = source;
            backup += ".bak";
            std::error_code ec;
            if (std::filesystem::exists(source, ec) && !std::filesystem::exists(backup, ec)) {
                std::filesystem::copy_file(source, backup, std::filesystem::copy_options::none, ec);
                if (ec) {
                    logger::warn("Pending impulses: could not create migration backup {}: {}", backup.string(),
                                 ec.message());
                } else {
                    logger::info("Pending impulses: retained one-time migration backup at {}", backup.string());
                }
            }
        }

        void WriteLocked()
        {
            if (g_loadedSaveId.empty()) {
                return;
            }
            if (g_migrated) {
                BackupLegacySidecar();
            }

            const auto path = FilePath();
            try {
                auto root = ReadFile();
                root["formatVersion"] = kFormatVersion;
                auto& saves = root["saves"];
                if (!saves.is_object()) {
                    saves = nlohmann::json::object();
                }
                const auto sequence = root.value("seq", 0ull) + 1;
                root["seq"] = sequence;

                nlohmann::json entries = nlohmann::json::array();
                for (const auto& entry : g_entries) {
                    entries.push_back(EntryToJson(entry));
                }
                nlohmann::json personal = nlohmann::json::array();
                for (const auto& slot : g_ledger) {
                    personal.push_back({ { "originEntryId", slot.originEntryId },
                                         { "formID", slot.formID },
                                         { "speakerName", slot.speakerName },
                                         { "topic", slot.topic },
                                         { "normalizedTopic", slot.normalizedTopic },
                                         { "lens", slot.lens },
                                         { "provisional", slot.provisional } });
                }
                nlohmann::json party = nlohmann::json::array();
                for (const auto& topic : g_party) {
                    party.push_back({ { "originEntryId", topic.originEntryId },
                                      { "speakerFormID", topic.speakerFormID },
                                      { "speakerName", topic.speakerName },
                                      { "target", TargetToJson(topic.target) },
                                      { "topic", topic.topic },
                                      { "normalizedTopic", topic.normalizedTopic },
                                      { "raisedGameDays", topic.raisedGameDays } });
                }

                saves[g_loadedSaveId] = { { "version", kFormatVersion },
                                          { "seq", sequence },
                                          { "nextEntryId", g_nextEntryId },
                                          { "nextEvidenceSequence", g_nextEvidenceSequence },
                                          { "entries", std::move(entries) },
                                          { "personalLedger", std::move(personal) },
                                          { "partyLedger", std::move(party) } };

                while (saves.size() > kMaxSaves) {
                    std::string oldestKey;
                    auto oldestSequence = std::numeric_limits<unsigned long long>::max();
                    for (const auto& [key, value] : saves.items()) {
                        const auto candidate = value.is_object() ? value.value("seq", 0ull) : 0ull;
                        if (candidate < oldestSequence) {
                            oldestSequence = candidate;
                            oldestKey = key;
                        }
                    }
                    if (oldestKey.empty()) {
                        break;
                    }
                    saves.erase(oldestKey);
                }

                std::error_code ec;
                std::filesystem::create_directories(path.parent_path(), ec);
                std::ofstream file{ path, std::ios::trunc };
                file << root.dump(2);
                if (!file) {
                    throw std::runtime_error("write failed");
                }
                g_dirty = false;
                g_migrated = false;
                g_lastWriteMs = NowMs();
            } catch (const std::exception& error) {
                logger::error("Pending impulses: failed to write {}: {}", path.string(), error.what());
            }
        }

        void LoadLocked(const nlohmann::json& save)
        {
            const bool legacy = save.is_object() && save.value("version", 0u) < kFormatVersion;
            const auto& entries = legacy ? save.value("impulses", nlohmann::json::array())
                                         : save.value("entries", nlohmann::json::array());
            if (entries.is_array()) {
                for (const auto& value : entries) {
                    if (!value.is_object()) {
                        continue;
                    }
                    auto entry = EntryFromJson(value, legacy);
                    if (entry.formID == 0 || entry.text.empty()) {
                        continue;
                    }
                    if (entry.id == 0) {
                        entry.id = g_nextEntryId++;
                    }
                    g_nextEntryId = std::max(g_nextEntryId, entry.id + 1);
                    g_nextEvidenceSequence = std::max(g_nextEvidenceSequence,
                                                      entry.lastAttemptedEvidenceSequence + 1);
                    g_entries.push_back(std::move(entry));
                }
            }

            const auto& personal = legacy ? save.value("ledger", nlohmann::json::array())
                                          : save.value("personalLedger", nlohmann::json::array());
            if (personal.is_array()) {
                for (const auto& value : personal) {
                    if (!value.is_object()) {
                        continue;
                    }
                    LedgerSlot slot;
                    slot.originEntryId = legacy ? 0 : value.value("originEntryId", 0ull);
                    slot.formID = value.value("formID", 0u);
                    slot.speakerName = value.value("speakerName", std::string{});
                    slot.topic = value.value("topic", std::string{});
                    slot.normalizedTopic = value.value("normalizedTopic", std::string{});
                    if (slot.normalizedTopic.empty()) {
                        if (const auto key = NormalizeTopic(slot.topic)) {
                            slot.normalizedTopic = *key;
                        }
                    }
                    slot.lens = value.value("lens", std::string{});
                    slot.provisional = value.value("provisional", true);
                    if (legacy) {
                        const auto owner = std::ranges::find_if(g_entries, [&](const Entry& entry) {
                            return entry.formID == slot.formID && entry.lens == slot.lens &&
                                   entry.normalizedTopic == slot.normalizedTopic;
                        });
                        slot.originEntryId = owner != g_entries.end() ? owner->id : g_nextEntryId++;
                    }
                    if (slot.formID != 0 && !slot.normalizedTopic.empty()) {
                        g_ledger.push_back(std::move(slot));
                    }
                }
            }

            if (!legacy) {
                const auto party = save.value("partyLedger", nlohmann::json::array());
                if (party.is_array()) {
                    for (const auto& value : party) {
                        if (!value.is_object()) {
                            continue;
                        }
                        PartyTopic topic;
                        topic.originEntryId = value.value("originEntryId", 0ull);
                        topic.speakerFormID = value.value("speakerFormID", 0u);
                        topic.speakerName = value.value("speakerName", std::string{});
                        topic.target = TargetFromJson(value.value("target", nlohmann::json{}));
                        topic.topic = value.value("topic", std::string{});
                        topic.normalizedTopic = value.value("normalizedTopic", std::string{});
                        topic.raisedGameDays = value.value("raisedGameDays", 0.0);
                        if (topic.originEntryId != 0 && !topic.normalizedTopic.empty()) {
                            g_party.push_back(std::move(topic));
                        }
                    }
                }
                g_nextEntryId = std::max(g_nextEntryId, save.value("nextEntryId", 1ull));
                g_nextEvidenceSequence = std::max(g_nextEvidenceSequence,
                                                  save.value("nextEvidenceSequence", 1ull));
            } else {
                g_migrated = true;
                g_dirty = true;
            }
        }
    }

    void CueOwnership::Coalesce(EntryId newestEntryId, TargetScope newestTarget)
    {
        entryId = newestEntryId;
        target = std::move(newestTarget);
        ++carries;
    }

    std::string_view ToString(LifecycleState state)
    {
        switch (state) {
        case LifecycleState::Untouched:
            return "untouched";
        case LifecycleState::RaisedUnmet:
            return "raised_unmet";
        case LifecycleState::Met:
            return "met";
        }
        return "untouched";
    }

    std::optional<LifecycleState> ParseLifecycleState(std::string_view value)
    {
        if (value == "untouched") {
            return LifecycleState::Untouched;
        }
        if (value == "raised_unmet") {
            return LifecycleState::RaisedUnmet;
        }
        if (value == "met") {
            return LifecycleState::Met;
        }
        return std::nullopt;
    }

    std::optional<std::string> NormalizeTopic(std::string_view topic)
    {
        if (topic.empty()) {
            return std::nullopt;
        }
        const auto wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, topic.data(),
                                                     static_cast<int>(topic.size()), nullptr, 0);
        if (wideLength <= 0) {
            return std::nullopt;
        }
        std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, topic.data(), static_cast<int>(topic.size()),
                                wide.data(), wideLength) != wideLength) {
            return std::nullopt;
        }

        const auto normalizedLength = NormalizeString(NormalizationKC, wide.data(), wideLength, nullptr, 0);
        if (normalizedLength <= 0) {
            return std::nullopt;
        }
        std::wstring normalized(static_cast<std::size_t>(normalizedLength), L'\0');
        if (NormalizeString(NormalizationKC, wide.data(), wideLength, normalized.data(), normalizedLength) <= 0) {
            return std::nullopt;
        }

        const auto lowerLength = LCMapStringEx(LOCALE_NAME_INVARIANT,
                                               LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
                                               normalized.data(), static_cast<int>(normalized.size()), nullptr, 0,
                                               nullptr, nullptr, 0);
        if (lowerLength <= 0) {
            return std::nullopt;
        }
        std::wstring lower(static_cast<std::size_t>(lowerLength), L'\0');
        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
                          normalized.data(), static_cast<int>(normalized.size()), lower.data(), lowerLength,
                          nullptr, nullptr, 0) <= 0) {
            return std::nullopt;
        }
        while (!lower.empty() && lower.back() == L'\0') {
            lower.pop_back();
        }

        std::vector<WORD> types(lower.size());
        if (!lower.empty() && !GetStringTypeW(CT_CTYPE1, lower.data(), static_cast<int>(lower.size()), types.data())) {
            return std::nullopt;
        }
        std::wstring key;
        key.reserve(lower.size());
        bool separator = false;
        for (std::size_t i = 0; i < lower.size(); ++i) {
            if ((types[i] & (C1_ALPHA | C1_DIGIT)) != 0) {
                if (separator && !key.empty()) {
                    key.push_back(L' ');
                }
                separator = false;
                key.push_back(lower[i]);
            } else {
                separator = true;
            }
        }
        if (key.empty()) {
            return std::nullopt;
        }

        const auto utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, key.data(),
                                                    static_cast<int>(key.size()), nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) {
            return std::nullopt;
        }
        std::string result(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, key.data(), static_cast<int>(key.size()),
                                result.data(), utf8Length, nullptr, nullptr) != utf8Length) {
            return std::nullopt;
        }
        return result;
    }

    EntryId Carry(Entry entry, std::size_t ledgerCap)
    {
        const auto rings = RingsSnapshot();
        if (const auto key = NormalizeTopic(entry.topic)) {
            entry.normalizedTopic = *key;
        } else if (!entry.topic.empty()) {
            logger::warn("Pending impulses: rejecting unusable topic key for '{}'", entry.topic);
        }

        std::scoped_lock lock{ g_lock };
        entry.id = g_nextEntryId++;
        entry.state = LifecycleState::Untouched;
        entry.raisedGameDays = 0.0;
        entry.lastAttemptedEvidenceSequence = g_nextEvidenceSequence == 0 ? 0 : g_nextEvidenceSequence - 1;
        entry.fallbackConsumed = false;
        entry.unverified = false;

        if (const auto old = std::ranges::find_if(g_entries, [&](const Entry& current) {
                return current.formID == entry.formID && current.lens == entry.lens;
            }); old != g_entries.end()) {
            RetireEntryLocked(old, false);
        }
        g_entries.push_back(entry);
        RecordProvisionalLocked(g_entries.back(), ledgerCap, rings);
        g_dirty = true;
        return entry.id;
    }

    bool MarkRaised(EntryId id, double raisedGameDays)
    {
        std::scoped_lock lock{ g_lock };
        auto* entry = FindLocked(id);
        return entry && RaiseLocked(*entry, raisedGameDays);
    }

    bool MarkMet(EntryId id, double metGameDays)
    {
        std::scoped_lock lock{ g_lock };
        auto it = std::ranges::find(g_entries, id, &Entry::id);
        if (it == g_entries.end() || it->state == LifecycleState::Met) {
            return false;
        }
        const auto raisedAt = it->raisedGameDays > 0.0 ? it->raisedGameDays : metGameDays;
        if (!RaiseLocked(*it, raisedAt)) {
            return false;
        }
        it = std::ranges::find(g_entries, id, &Entry::id);
        if (it == g_entries.end()) {
            return false;
        }
        it->state = LifecycleState::Met;
        g_entries.erase(it);
        g_dirty = true;
        return true;
    }

    bool MarkFallbackConsumed(EntryId id)
    {
        std::scoped_lock lock{ g_lock };
        auto* entry = FindLocked(id);
        if (!entry || entry->fallbackConsumed) {
            return false;
        }
        entry->fallbackConsumed = true;
        g_dirty = true;
        return true;
    }

    bool StopCarrying(EntryId id, std::string_view reason)
    {
        std::scoped_lock lock{ g_lock };
        const auto it = std::ranges::find(g_entries, id, &Entry::id);
        if (it == g_entries.end()) {
            return false;
        }
        logger::info("Pending impulse {}: stop carrying ({})", id, reason.empty() ? "no reason" : reason);
        RetireEntryLocked(it, false);
        return true;
    }

    bool ForgetSubject(EntryId id, std::string_view reason)
    {
        std::scoped_lock lock{ g_lock };
        const auto it = std::ranges::find(g_entries, id, &Entry::id);
        if (it != g_entries.end()) {
            logger::info("Pending impulse {}: forget subject ({})", id, reason.empty() ? "no reason" : reason);
            RetireEntryLocked(it, true);
            return true;
        }
        const auto before = g_ledger.size() + g_party.size();
        RemoveOwnedMemoryLocked(id);
        if (g_ledger.size() + g_party.size() != before) {
            g_dirty = true;
            return true;
        }
        return false;
    }

    std::size_t ForgetAll(std::string_view reason)
    {
        std::scoped_lock lock{ g_lock };
        const auto count = g_entries.size();
        g_entries.clear();
        g_ledger.clear();
        g_party.clear();
        g_dirty = true;
        logger::info("Pending impulses: forgot all state ({})", reason.empty() ? "no reason" : reason);
        return count;
    }

    std::size_t StopCarryingActor(std::uint32_t formID, std::string_view reason)
    {
        std::scoped_lock lock{ g_lock };
        std::size_t removed = 0;
        for (auto it = g_entries.begin(); it != g_entries.end();) {
            if (it->formID == formID) {
                if (it->state == LifecycleState::Untouched) {
                    RemoveOwnedProvisionalLocked(it->id);
                }
                it = g_entries.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (removed > 0) {
            g_dirty = true;
            logger::info("Pending impulses: stopped carrying {} entr(ies) for {:08X} ({})", removed, formID,
                         reason.empty() ? "no reason" : reason);
        }
        return removed;
    }

    std::optional<Entry> Find(EntryId id)
    {
        std::scoped_lock lock{ g_lock };
        if (const auto* entry = FindLocked(id)) {
            return *entry;
        }
        return std::nullopt;
    }

    std::vector<Entry> Snapshot()
    {
        std::scoped_lock lock{ g_lock };
        return g_entries;
    }

    std::size_t Count()
    {
        std::scoped_lock lock{ g_lock };
        return g_entries.size();
    }

    std::string Get(std::uint32_t formID)
    {
        const auto owner = FloorOwner(formID);
        if (!owner) {
            return {};
        }
        std::scoped_lock lock{ g_lock };
        const auto* entry = FindLocked(owner->entryId);
        if (!entry || entry->formID != formID || entry->unverified ||
            entry->state != LifecycleState::Untouched) {
            return {};
        }
        return "- " + entry->text;
    }

    std::string GetBackground(std::uint32_t formID)
    {
        const auto owner = FloorOwner(formID);
        const auto ownerId = owner ? owner->entryId : 0;
        std::scoped_lock lock{ g_lock };
        std::string out;
        for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it) {
            if (it->formID != formID || it->unverified || it->state != LifecycleState::Untouched ||
                it->id == ownerId) {
                continue;
            }
            out += out.empty() ? "- " : "\n- ";
            out += it->text;
        }
        return out;
    }

    std::string GetSpoken(std::uint32_t formID)
    {
        std::scoped_lock lock{ g_lock };
        return RenderLocked(formID, LifecycleState::RaisedUnmet);
    }

    std::string State(std::uint32_t formID)
    {
        std::scoped_lock lock{ g_lock };
        if (std::ranges::any_of(g_entries, [&](const Entry& entry) {
                return entry.formID == formID && !entry.unverified && entry.state == LifecycleState::Untouched;
            })) {
            return "carried";
        }
        if (std::ranges::any_of(g_entries, [&](const Entry& entry) {
                return entry.formID == formID && !entry.unverified && entry.state == LifecycleState::RaisedUnmet;
            })) {
            return "spoken";
        }
        return {};
    }

    void GrantFloor(EntryId id, std::uint32_t speakerFormID, TargetScope target, std::uint64_t speakerId)
    {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{ g_floorLock };
        std::erase_if(g_floor, [&](const FloorGrant& grant) { return grant.deadline <= now; });
        if (std::ranges::any_of(g_floor, [&](const FloorGrant& grant) {
                return grant.speakerFormID == speakerFormID;
            })) {
            return;
        }
        g_floor.push_back({ id, speakerId, speakerFormID, std::move(target), now, now + kFloorWindow });
    }

    std::optional<FloorGrant> FloorOwner(std::uint32_t speakerFormID,
                                         std::chrono::steady_clock::time_point now)
    {
        std::scoped_lock lock{ g_floorLock };
        std::erase_if(g_floor, [&](const FloorGrant& grant) { return grant.deadline <= now; });
        const auto it = std::ranges::find(g_floor, speakerFormID, &FloorGrant::speakerFormID);
        return it == g_floor.end() ? std::nullopt : std::optional<FloorGrant>{ *it };
    }

    std::vector<FloorGrant> FloorSnapshot(std::chrono::steady_clock::time_point now)
    {
        std::scoped_lock lock{ g_floorLock };
        std::erase_if(g_floor, [&](const FloorGrant& grant) { return grant.deadline <= now; });
        return g_floor;
    }

    std::string HasTheFloor(std::uint32_t speakerFormID)
    {
        return FloorOwner(speakerFormID) ? "1" : "";
    }
    std::optional<EntryId> MarkFloorOwnerRaisedByIdentity(std::uint64_t speakerId,
                                                          std::int64_t arrivalMs,
                                                          double raisedGameDays)
    {
        if (speakerId == 0 || arrivalMs < 0) {
            return std::nullopt;
        }
        EntryId ownerId = 0;
        {
            std::scoped_lock lock{ g_floorLock };
            std::erase_if(g_floor, [&](const FloorGrant& grant) {
                const auto deadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            grant.deadline.time_since_epoch())
                                            .count();
                return arrivalMs >= deadlineMs;
            });
            const auto owner = std::ranges::find(g_floor, speakerId, &FloorGrant::speakerId);
            if (owner == g_floor.end()) {
                return std::nullopt;
            }
            const auto grantedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       owner->grantedAt.time_since_epoch())
                                       .count();
            const auto deadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        owner->deadline.time_since_epoch())
                                        .count();
            if (arrivalMs < grantedMs || arrivalMs >= deadlineMs) {
                return std::nullopt;
            }
            ownerId = owner->entryId;
            g_floor.erase(owner);
        }
        return MarkRaised(ownerId, raisedGameDays) ? std::optional{ ownerId } : std::nullopt;
    }

    bool MarkFloorOwnerRaised(std::uint32_t speakerFormID, double raisedGameDays)
    {
        const auto owner = FloorOwner(speakerFormID);
        if (!owner) {
            return false;
        }
        return MarkRaised(owner->entryId, raisedGameDays);
    }

    void CloseFloor(std::uint32_t speakerFormID)
    {
        std::scoped_lock lock{ g_floorLock };
        std::erase_if(g_floor, [&](const FloorGrant& grant) { return grant.speakerFormID == speakerFormID; });
    }

    bool LedgerSuppresses(std::uint32_t formID, std::string_view topic)
    {
        const auto key = NormalizeTopic(topic);
        if (!key) {
            return false;
        }
        std::scoped_lock lock{ g_lock };
        return std::ranges::any_of(g_ledger, [&](const LedgerSlot& slot) {
            return slot.formID == formID && slot.normalizedTopic == *key;
        });
    }

    bool PartySuppresses(std::string_view topic, const TargetScope& target)
    {
        const auto key = NormalizeTopic(topic);
        if (!key) {
            return false;
        }
        std::scoped_lock lock{ g_lock };
        return std::ranges::any_of(g_party, [&](const PartyTopic& record) {
            return record.normalizedTopic == *key && CompatibleTargets(record.target, target);
        });
    }

    std::vector<std::string> LedgerTopics(std::uint32_t formID)
    {
        std::scoped_lock lock{ g_lock };
        std::vector<std::string> result;
        for (const auto& slot : g_ledger) {
            if (slot.formID == formID) {
                result.push_back(slot.topic);
            }
        }
        return result;
    }

    std::vector<LedgerSlot> LedgerSnapshot()
    {
        std::scoped_lock lock{ g_lock };
        return g_ledger;
    }

    std::vector<PartyTopic> PartySnapshot()
    {
        std::scoped_lock lock{ g_lock };
        return g_party;
    }

    std::vector<PartyTopic> PartyPromptSnapshot()
    {
        std::scoped_lock lock{ g_lock };
        const auto first = g_party.size() > kPartyPromptCap ? g_party.size() - kPartyPromptCap : 0;
        return { g_party.begin() + static_cast<std::ptrdiff_t>(first), g_party.end() };
    }

    void SetLedgerCap(std::size_t cap)
    {
        g_cap.store(std::max<std::size_t>(cap, 1));
    }

    void SetLensRings(std::vector<LensRing> lenses)
    {
        std::scoped_lock lock{ g_ringLock };
        g_rings = std::move(lenses);
    }

    void SetPartyEchoGameDays(float days)
    {
        g_partyEchoGameDays.store(std::max(days, 0.0f));
    }

    void Expire(double nowGameDays, float ttlGameMinutes,
                std::span<const EntryId> protectedEntries)
    {
        std::scoped_lock lock{ g_lock };
        if (ttlGameMinutes > 0.0f) {
            const auto ttlDays = static_cast<double>(ttlGameMinutes) / (24.0 * 60.0);
            for (auto it = g_entries.begin(); it != g_entries.end();) {
                if (std::ranges::find(protectedEntries, it->id) != protectedEntries.end()) {
                    ++it;
                    continue;
                }
                const auto age = nowGameDays - AgeAnchor(*it);
                if (age >= 0.0 && age >= ttlDays) {
                    if (it->state == LifecycleState::Untouched) {
                        RemoveOwnedProvisionalLocked(it->id);
                    }
                    it = g_entries.erase(it);
                    g_dirty = true;
                } else {
                    ++it;
                }
            }
        }

        const auto retention = static_cast<double>(g_partyEchoGameDays.load());
        if (retention >= 0.0) {
            const auto before = g_party.size();
            std::erase_if(g_party, [&](const PartyTopic& topic) {
                const auto age = nowGameDays - topic.raisedGameDays;
                return age >= 0.0 && age > retention;
            });
            g_dirty = g_dirty || before != g_party.size();
        }
    }

    bool SetLastAttemptedEvidenceSequence(EntryId id, EvidenceSequence sequence)
    {
        std::scoped_lock lock{ g_lock };
        auto* entry = FindLocked(id);
        if (!entry || sequence <= entry->lastAttemptedEvidenceSequence) {
            return false;
        }
        entry->lastAttemptedEvidenceSequence = sequence;
        g_nextEvidenceSequence = std::max(g_nextEvidenceSequence, sequence + 1);
        g_dirty = true;
        return true;
    }

    EvidenceSequence NextEvidenceSequence()
    {
        std::scoped_lock lock{ g_lock };
        const auto result = g_nextEvidenceSequence++;
        g_dirty = true;
        return result;
    }

    EvidenceSequence EvidenceSequenceWatermark()
    {
        std::scoped_lock lock{ g_lock };
        return g_nextEvidenceSequence == 0 ? 0 : g_nextEvidenceSequence - 1;
    }

    std::vector<Entry> TakeUnverified()
    {
        std::scoped_lock lock{ g_lock };
        std::vector<Entry> result;
        for (auto& entry : g_entries) {
            if (entry.unverified) {
                entry.unverified = false;
                result.push_back(entry);
            }
        }
        return result;
    }

    void FlushPersistence()
    {
        std::scoped_lock lock{ g_lock };
        if (g_dirty) {
            WriteLocked();
        }
    }

    void Reset()
    {
        {
            std::scoped_lock lock{ g_lock };
            g_entries.clear();
            g_ledger.clear();
            g_party.clear();
            g_nextEntryId = 1;
            g_nextEvidenceSequence = 1;
            g_loadedSaveId.clear();
            g_dirty = false;
            g_migrated = false;
            g_lastWriteMs = 0;
        }
        {
            std::scoped_lock lock{ g_floorLock };
            g_floor.clear();
        }
    }

    void SyncPersistence(const std::string& saveId, double nowGameDays, float ttlGameMinutes)
    {
        if (saveId.empty()) {
            return;
        }
        bool loaded = false;
        {
            std::scoped_lock lock{ g_lock };
            if (g_loadedSaveId != saveId) {
                g_entries.clear();
                g_ledger.clear();
                g_party.clear();
                g_nextEntryId = 1;
                g_nextEvidenceSequence = 1;
                g_loadedSaveId = saveId;
                g_dirty = false;
                g_migrated = false;
                const auto root = ReadFile();
                const auto saves = root.value("saves", nlohmann::json::object());
                if (saves.is_object() && saves.contains(saveId) && saves[saveId].is_object()) {
                    LoadLocked(saves[saveId]);
                }
                loaded = true;
            }
        }
        if (loaded) {
            std::scoped_lock floorLock{ g_floorLock };
            g_floor.clear();
        }
        Expire(nowGameDays, ttlGameMinutes);
        std::scoped_lock lock{ g_lock };
        if (g_dirty && (g_migrated || NowMs() - g_lastWriteMs >= kWriteIntervalMs)) {
            WriteLocked();
        }
    }

    std::filesystem::path FilePath()
    {
        return std::filesystem::path{ "Data/SKSE/Plugins/AgencyEngine_Pending.json" };
    }
}
