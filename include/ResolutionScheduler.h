#pragma once

#include "PendingImpulse.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace AgencyEngine::ResolutionScheduling
{
    struct ActiveSaveToken
    {
        std::string saveId;
        std::uint64_t generation = 0;
        bool operator==(const ActiveSaveToken&) const = default;
    };

    struct RawEvent
    {
        std::string sourceId;
        std::uint64_t actorId = 0;
        std::uint64_t targetId = 0;
        std::string type;
        std::string text;
        std::int64_t arrivalMs = 0;
        PendingImpulses::EntryId raisedEntryId = 0;
    };

    struct EvidenceEvent : RawEvent
    {
        PendingImpulses::EvidenceSequence sequence = 0;
    };

    enum class Trigger
    {
        None,
        EventCheckpoint,
        Manual,
        PreExpiry,
    };

    std::string_view ToString(Trigger trigger);

    struct BatchToken
    {
        ActiveSaveToken save;
        std::uint64_t batchId = 0;
        bool operator==(const BatchToken&) const = default;
    };

    struct BatchEntry
    {
        PendingImpulses::Entry entry;
        std::vector<PendingImpulses::EvidenceSequence> relevantEventIds;
    };

    struct Batch
    {
        BatchToken token;
        Trigger trigger = Trigger::None;
        std::vector<BatchEntry> entries;
        std::vector<EvidenceEvent> events;
        PendingImpulses::EvidenceSequence upperSequence = 0;
    };

    struct CompletedResult
    {
        Batch batch;
        std::string response;
        bool success = false;
    };

    struct ParsedVerdict
    {
        PendingImpulses::EntryId id = 0;
        PendingImpulses::LifecycleState state = PendingImpulses::LifecycleState::Untouched;
    };

    struct Diagnostics
    {
        std::size_t queuedRaw = 0;
        std::size_t acceptedEvidence = 0;
        std::size_t eligibleEntries = 0;
        bool batchInFlight = false;
        Trigger lastTrigger = Trigger::None;
        std::uint64_t paidBatches = 0;
        std::uint64_t staleResults = 0;
        std::uint64_t queueOverflow = 0;
    };

    class Scheduler
    {
    public:
        explicit Scheduler(std::size_t rawCapacity = 256);

        ActiveSaveToken BeginSave(std::string saveId);
        ActiveSaveToken ActiveToken() const;
        void Enqueue(RawEvent event);
        void QueueManual(std::span<const PendingImpulses::EntryId> ids);
        void QueueFallback(std::span<const PendingImpulses::EntryId> ids);

        std::optional<Batch> TryDispatch(
            std::span<const PendingImpulses::Entry> entries,
            int eventInterval,
            std::int64_t automaticCooldownMs,
            std::int64_t nowMs,
            const std::function<PendingImpulses::EvidenceSequence()>& allocateSequence);

        void SubmitResult(BatchToken token, std::string response, bool success);
        std::optional<CompletedResult> TakeResult();
        void CancelInFlight();

        Diagnostics Snapshot() const;
        std::optional<Batch> InFlight() const;
        std::vector<PendingImpulses::EntryId> QueuedFallbacks() const;

        static std::vector<ParsedVerdict> ParseVerdicts(std::string_view response,
                                                        const Batch& batch);

    private:
        struct StampedRaw
        {
            ActiveSaveToken token;
            RawEvent event;
        };
        struct StampedResult
        {
            BatchToken token;
            std::string response;
            bool success = false;
        };

        void DrainRaw(const std::function<PendingImpulses::EvidenceSequence()>& allocateSequence);

        const std::size_t rawCapacity_;
        mutable std::mutex lock_;
        ActiveSaveToken active_;
        std::uint64_t nextGeneration_ = 1;
        std::uint64_t nextBatchId_ = 1;
        std::deque<StampedRaw> raw_;
        std::deque<StampedResult> results_;
        std::vector<EvidenceEvent> evidence_;
        std::set<std::string> sourceIds_;
        std::deque<std::string> sourceOrder_;
        std::set<PendingImpulses::EntryId> manual_;
        std::set<PendingImpulses::EntryId> fallback_;
        std::optional<Batch> inFlight_;
        bool hasAutomaticDispatch_ = false;
        std::int64_t lastAutomaticDispatchMs_ = 0;
        Diagnostics diagnostics_;
    };
}
