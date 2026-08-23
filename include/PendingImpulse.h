#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AgencyEngine::PendingImpulses
{
    inline constexpr auto kDecoratorName = "agencyengine_pending_impulse";
    inline constexpr auto kBackgroundDecoratorName = "agencyengine_pending_background";
    inline constexpr auto kSpokenDecoratorName = "agencyengine_pending_spoken";
    inline constexpr auto kFloorDecoratorName = "agencyengine_has_the_floor";
    inline constexpr auto kStateDecoratorName = "agencyengine_pending_state";

    using EntryId = std::uint64_t;
    using EvidenceSequence = std::uint64_t;

    enum class LifecycleState : std::uint8_t
    {
        Untouched,
        RaisedUnmet,
        Met,
    };

    std::string_view ToString(LifecycleState state);
    std::optional<LifecycleState> ParseLifecycleState(std::string_view value);

    struct TargetScope
    {
        std::uint64_t id = 0;
        std::string name;

        bool operator==(const TargetScope&) const = default;
    };

    struct Entry
    {
        EntryId id = 0;
        std::uint32_t formID = 0;
        std::uint64_t speakerId = 0;
        std::string speakerName;
        TargetScope target;
        std::string text;
        std::string topic;
        std::string normalizedTopic;
        std::string lens;
        bool proposal = false;
        LifecycleState state = LifecycleState::Untouched;
        double createdGameDays = 0.0;
        double raisedGameDays = 0.0;
        EvidenceSequence lastAttemptedEvidenceSequence = 0;
        bool fallbackConsumed = false;
        bool unverified = false;
    };

    struct LedgerSlot
    {
        EntryId originEntryId = 0;
        std::uint32_t formID = 0;
        std::string speakerName;
        std::string topic;
        std::string normalizedTopic;
        std::string lens;
        bool provisional = true;
    };

    struct PartyTopic
    {
        EntryId originEntryId = 0;
        std::uint32_t speakerFormID = 0;
        std::string speakerName;
        TargetScope target;
        std::string topic;
        std::string normalizedTopic;
        double raisedGameDays = 0.0;
    };

    struct FloorGrant
    {
        EntryId entryId = 0;
        std::uint64_t speakerId = 0;
        std::uint32_t speakerFormID = 0;
        TargetScope target;
        std::chrono::steady_clock::time_point grantedAt{};
        std::chrono::steady_clock::time_point deadline{};
    };

    inline constexpr std::chrono::milliseconds kFloorWindow{ 30000 };
    inline constexpr std::size_t kPartyLedgerCap = 32;
    struct CueOwnership
    {
        EntryId entryId = 0;
        TargetScope target;
        int carries = 0;

        void Coalesce(EntryId newestEntryId, TargetScope newestTarget);
    };

    inline constexpr std::size_t kPartyPromptCap = 12;

    struct LensRing
    {
        std::string name;
        std::size_t slots = 0;
    };

    // Carry allocates a never-reused save-local ID and atomically owns its
    // provisional personal-memory record. Replacing the same actor/lens retires
    // only the replaced entry and its provisional record.
    EntryId Carry(Entry entry, std::size_t ledgerCap = 0);

    // ID-based monotonic lifecycle commands. Missing IDs and regressions are
    // rejected without touching a replacement entry in the same actor/lens.
    bool MarkRaised(EntryId id, double raisedGameDays);
    bool MarkMet(EntryId id, double metGameDays);
    bool MarkFallbackConsumed(EntryId id);
    bool StopCarrying(EntryId id, std::string_view reason = {});
    bool ForgetSubject(EntryId id, std::string_view reason = {});
    std::size_t ForgetAll(std::string_view reason = {});
    std::size_t StopCarryingActor(std::uint32_t formID, std::string_view reason = {});

    std::optional<Entry> Find(EntryId id);
    std::vector<Entry> Snapshot();
    std::size_t Count();

    std::string GetBackground(std::uint32_t formID);
    std::string Get(std::uint32_t formID);
    std::string GetSpoken(std::uint32_t formID);
    std::string State(std::uint32_t formID);
    void GrantFloor(EntryId id, std::uint32_t speakerFormID, TargetScope target = {},
                    std::uint64_t speakerId = 0);
    std::optional<FloorGrant> FloorOwner(
        std::uint32_t speakerFormID,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::vector<FloorGrant> FloorSnapshot(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::string HasTheFloor(std::uint32_t speakerFormID);
    std::optional<EntryId> MarkFloorOwnerRaisedByIdentity(std::uint64_t speakerId,
                                                          std::int64_t arrivalMs,
                                                          double raisedGameDays);
    bool MarkFloorOwnerRaised(std::uint32_t speakerFormID, double raisedGameDays);
    void CloseFloor(std::uint32_t speakerFormID);

    std::optional<std::string> NormalizeTopic(std::string_view topic);
    bool LedgerSuppresses(std::uint32_t formID, std::string_view topic);
    bool PartySuppresses(std::string_view topic, const TargetScope& target);
    std::vector<std::string> LedgerTopics(std::uint32_t formID);
    std::vector<LedgerSlot> LedgerSnapshot();
    std::vector<PartyTopic> PartySnapshot();
    std::vector<PartyTopic> PartyPromptSnapshot();

    void SetLedgerCap(std::size_t cap);
    void SetLensRings(std::vector<LensRing> lenses);
    void SetPartyEchoGameDays(float days);
    void Expire(double nowGameDays, float ttlGameMinutes,
                std::span<const EntryId> protectedEntries = {});

    bool SetLastAttemptedEvidenceSequence(EntryId id, EvidenceSequence sequence);
    EvidenceSequence NextEvidenceSequence();
    EvidenceSequence EvidenceSequenceWatermark();

    std::vector<Entry> TakeUnverified();
    void FlushPersistence();
    void Reset();
    void SyncPersistence(const std::string& saveId, double nowGameDays, float ttlGameMinutes);
    std::filesystem::path FilePath();
}
