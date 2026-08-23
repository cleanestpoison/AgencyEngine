#include "ResolutionScheduler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ranges>
#include <sstream>

namespace AgencyEngine::ResolutionScheduling
{
    namespace
    {
        bool IsGameplay(std::string_view type)
        {
            return type.find("combat") != std::string_view::npos || type.find("hit") != std::string_view::npos ||
                   type.find("activity") != std::string_view::npos || type.find("spell") != std::string_view::npos;
        }

        std::set<std::string> MeaningfulTokens(std::string_view text)
        {
            static const std::set<std::string> stop{ "a", "an", "and", "at", "for", "in", "it", "of", "on",
                                                     "the", "to", "we", "you" };
            std::set<std::string> result;
            const auto normalized = PendingImpulses::NormalizeTopic(text);
            if (!normalized) {
                return result;
            }
            std::istringstream stream{ *normalized };
            for (std::string token; stream >> token;) {
                if (token.size() > 1 && !stop.contains(token)) {
                    result.insert(std::move(token));
                }
            }
            return result;
        }

        bool SpeakerParticipates(const PendingImpulses::Entry& entry, const EvidenceEvent& event)
        {
            return entry.speakerId == 0 || event.actorId == entry.speakerId ||
                   event.targetId == entry.speakerId;
        }

        bool DirectExchange(const PendingImpulses::Entry& entry, const EvidenceEvent& event)
        {
            if (!SpeakerParticipates(entry, event)) {
                return false;
            }
            if (entry.target.id == 0 || entry.speakerId == 0) {
                return true;
            }
            return (event.actorId == entry.speakerId && event.targetId == entry.target.id) ||
                   (event.actorId == entry.target.id && event.targetId == entry.speakerId);
        }

        bool Relevant(const PendingImpulses::Entry& entry, const EvidenceEvent& event)
        {
            if (event.raisedEntryId == entry.id || !SpeakerParticipates(entry, event)) {
                return false;
            }
            if (entry.proposal && IsGameplay(event.type)) {
                return true;
            }
            if (entry.state == PendingImpulses::LifecycleState::RaisedUnmet &&
                event.type.starts_with("dialogue")) {
                return DirectExchange(entry, event);
            }
            const auto topic = MeaningfulTokens(entry.topic);
            const auto text = MeaningfulTokens(event.text);
            if (topic.empty() || text.empty()) {
                return false;
            }
            return std::ranges::any_of(topic, [&](const std::string& token) { return text.contains(token); });
        }
    }

    std::string_view ToString(Trigger trigger)
    {
        switch (trigger) {
        case Trigger::EventCheckpoint:
            return "event_checkpoint";
        case Trigger::Manual:
            return "manual";
        case Trigger::PreExpiry:
            return "pre_expiry";
        default:
            return "none";
        }
    }

    Scheduler::Scheduler(std::size_t rawCapacity) : rawCapacity_(std::max<std::size_t>(rawCapacity, 1)) {}

    ActiveSaveToken Scheduler::BeginSave(std::string saveId)
    {
        std::scoped_lock lock{ lock_ };
        active_ = { std::move(saveId), nextGeneration_++ };
        raw_.clear();
        results_.clear();
        evidence_.clear();
        sourceIds_.clear();
        sourceOrder_.clear();
        manual_.clear();
        fallback_.clear();
        inFlight_.reset();
        hasAutomaticDispatch_ = false;
        lastAutomaticDispatchMs_ = 0;
        diagnostics_.queuedRaw = 0;
        diagnostics_.acceptedEvidence = 0;
        diagnostics_.eligibleEntries = 0;
        diagnostics_.batchInFlight = false;
        return active_;
    }

    ActiveSaveToken Scheduler::ActiveToken() const
    {
        std::scoped_lock lock{ lock_ };
        return active_;
    }

    void Scheduler::Enqueue(RawEvent event)
    {
        std::scoped_lock lock{ lock_ };
        if (raw_.size() == rawCapacity_) {
            raw_.pop_front();
            ++diagnostics_.queueOverflow;
        }
        raw_.push_back({ active_, std::move(event) });
        diagnostics_.queuedRaw = raw_.size();
    }

    void Scheduler::QueueManual(std::span<const PendingImpulses::EntryId> ids)
    {
        std::scoped_lock lock{ lock_ };
        for (const auto id : ids) {
            const bool alreadyInFlight =
                inFlight_ && std::ranges::any_of(inFlight_->entries,
                    [&](const BatchEntry& entry) { return entry.entry.id == id; });
            if (!alreadyInFlight) {
                manual_.insert(id);
            }
        }
    }

    void Scheduler::QueueFallback(std::span<const PendingImpulses::EntryId> ids)
    {
        std::scoped_lock lock{ lock_ };
        for (const auto id : ids) {
            const bool alreadyInFlight =
                inFlight_ && std::ranges::any_of(inFlight_->entries,
                    [&](const BatchEntry& entry) { return entry.entry.id == id; });
            if (!alreadyInFlight) {
                fallback_.insert(id);
            }
        }
    }

    void Scheduler::DrainRaw(const std::function<PendingImpulses::EvidenceSequence()>& allocateSequence)
    {
        while (!raw_.empty()) {
            auto raw = std::move(raw_.front());
            raw_.pop_front();
            if (raw.token != active_) {
                continue;
            }
            if (!raw.event.sourceId.empty()) {
                if (!sourceIds_.insert(raw.event.sourceId).second) {
                    continue;
                }
                sourceOrder_.push_back(raw.event.sourceId);
                while (sourceOrder_.size() > rawCapacity_ * 4) {
                    sourceIds_.erase(sourceOrder_.front());
                    sourceOrder_.pop_front();
                }
            }
            EvidenceEvent event;
            static_cast<RawEvent&>(event) = std::move(raw.event);
            event.sequence = allocateSequence();
            evidence_.push_back(std::move(event));
            if (evidence_.size() > 200) {
                evidence_.erase(evidence_.begin());
            }
        }
        diagnostics_.queuedRaw = 0;
        diagnostics_.acceptedEvidence = evidence_.size();
    }

    std::optional<Batch> Scheduler::TryDispatch(
        std::span<const PendingImpulses::Entry> entries,
        int eventInterval,
        std::int64_t automaticCooldownMs,
        std::int64_t nowMs,
        const std::function<PendingImpulses::EvidenceSequence()>& allocateSequence)
    {
        std::scoped_lock lock{ lock_ };
        DrainRaw(allocateSequence);
        if (inFlight_) {
            diagnostics_.batchInFlight = true;
            return std::nullopt;
        }

        const auto interval = static_cast<PendingImpulses::EvidenceSequence>(std::max(eventInterval, 1));
        const auto latestSequence = evidence_.empty() ? PendingImpulses::EvidenceSequence{ 0 } :
                                                        evidence_.back().sequence;
        const auto contextCount = static_cast<std::size_t>(std::max(eventInterval, 1));
        const auto firstEvidence = evidence_.size() > contextCount ? evidence_.size() - contextCount : 0;
        const auto window = std::span{ evidence_ }.subspan(firstEvidence);

        const bool hasManual = std::ranges::any_of(
            entries, [&](const PendingImpulses::Entry& entry) {
                return !entry.unverified && manual_.contains(entry.id);
            });
        const auto automaticallyDue = [&](const PendingImpulses::Entry& entry) {
            return latestSequence >= entry.lastAttemptedEvidenceSequence &&
                   latestSequence - entry.lastAttemptedEvidenceSequence >= interval;
        };

        std::vector<BatchEntry> eligible;
        for (const auto& entry : entries) {
            if (entry.unverified) {
                continue;
            }
            const bool selected = hasManual ? manual_.contains(entry.id) :
                                              (fallback_.contains(entry.id) || automaticallyDue(entry));
            if (!selected) {
                continue;
            }

            auto relevanceAfter = entry.lastAttemptedEvidenceSequence;
            if (entry.state == PendingImpulses::LifecycleState::RaisedUnmet) {
                for (const auto& event : window) {
                    if (event.raisedEntryId == entry.id) {
                        relevanceAfter = std::max(relevanceAfter, event.sequence);
                    }
                }
            }
            BatchEntry item{ entry, {} };
            for (const auto& event : window) {
                if (event.sequence > relevanceAfter && Relevant(entry, event)) {
                    item.relevantEventIds.push_back(event.sequence);
                }
            }
            eligible.push_back(std::move(item));
        }
        diagnostics_.eligibleEntries = eligible.size();
        if (eligible.empty()) {
            return std::nullopt;
        }

        Trigger trigger = Trigger::Manual;
        if (!hasManual) {
            const auto cooldown = std::max<std::int64_t>(automaticCooldownMs, 0);
            if (hasAutomaticDispatch_ &&
                (nowMs < lastAutomaticDispatchMs_ || nowMs - lastAutomaticDispatchMs_ < cooldown)) {
                return std::nullopt;
            }
            trigger = std::ranges::any_of(
                          eligible, [&](const BatchEntry& entry) {
                              return fallback_.contains(entry.entry.id);
                          })
                          ? Trigger::PreExpiry
                          : Trigger::EventCheckpoint;
        }

        Batch batch;
        batch.token = { active_, nextBatchId_++ };
        batch.trigger = trigger;
        batch.entries = std::move(eligible);
        batch.events.assign(evidence_.begin() + static_cast<std::ptrdiff_t>(firstEvidence), evidence_.end());
        batch.upperSequence = latestSequence;
        for (const auto& entry : batch.entries) {
            manual_.erase(entry.entry.id);
            fallback_.erase(entry.entry.id);
        }
        if (!hasManual) {
            hasAutomaticDispatch_ = true;
            lastAutomaticDispatchMs_ = nowMs;
        }
        inFlight_ = batch;
        diagnostics_.batchInFlight = true;
        diagnostics_.lastTrigger = trigger;
        ++diagnostics_.paidBatches;
        return batch;
    }

    void Scheduler::SubmitResult(BatchToken token, std::string response, bool success)
    {
        std::scoped_lock lock{ lock_ };
        results_.push_back({ std::move(token), std::move(response), success });
    }

    std::optional<CompletedResult> Scheduler::TakeResult()
    {
        std::scoped_lock lock{ lock_ };
        while (!results_.empty()) {
            auto result = std::move(results_.front());
            results_.pop_front();
            if (!inFlight_ || result.token != inFlight_->token || result.token.save != active_) {
                ++diagnostics_.staleResults;
                continue;
            }
            CompletedResult completed{ *inFlight_, std::move(result.response), result.success };
            inFlight_.reset();
            diagnostics_.batchInFlight = false;
            return completed;
        }
        return std::nullopt;
    }

    void Scheduler::CancelInFlight()
    {
        std::scoped_lock lock{ lock_ };
        inFlight_.reset();
        diagnostics_.batchInFlight = false;
    }

    Diagnostics Scheduler::Snapshot() const
    {
        std::scoped_lock lock{ lock_ };
        auto result = diagnostics_;
        result.queuedRaw = raw_.size();
        result.acceptedEvidence = evidence_.size();
        result.batchInFlight = inFlight_.has_value();
        return result;
    }

    std::optional<Batch> Scheduler::InFlight() const
    {
        std::scoped_lock lock{ lock_ };
        return inFlight_;
    }
    std::vector<PendingImpulses::EntryId> Scheduler::QueuedFallbacks() const
    {
        std::scoped_lock lock{ lock_ };
        return { fallback_.begin(), fallback_.end() };
    }


    std::vector<ParsedVerdict> Scheduler::ParseVerdicts(std::string_view response, const Batch& batch)
    {
        std::vector<ParsedVerdict> result;
        try {
            const auto open = response.find('{');
            const auto close = response.rfind('}');
            if (open == std::string_view::npos || close <= open) {
                return result;
            }
            const auto root = nlohmann::json::parse(response.substr(open, close - open + 1));
            if (!root.is_object() || !root.contains("verdicts") || !root["verdicts"].is_array()) {
                return result;
            }
            std::set<PendingImpulses::EntryId> duplicates;
            std::set<PendingImpulses::EntryId> seen;
            for (const auto& value : root["verdicts"]) {
                if (!value.is_object() || !value.contains("id") || !value["id"].is_number_unsigned() ||
                    !value.contains("status") || !value["status"].is_string()) {
                    continue;
                }
                const auto id = value["id"].get<PendingImpulses::EntryId>();
                if (!seen.insert(id).second) {
                    duplicates.insert(id);
                    continue;
                }
                const auto requested = std::ranges::find_if(batch.entries,
                    [&](const BatchEntry& entry) { return entry.entry.id == id; });
                const auto state = PendingImpulses::ParseLifecycleState(value["status"].get<std::string>());
                if (requested == batch.entries.end() || !state ||
                    static_cast<int>(*state) < static_cast<int>(requested->entry.state)) {
                    continue;
                }
                result.push_back({ id, *state });
            }
            std::erase_if(result, [&](const ParsedVerdict& verdict) { return duplicates.contains(verdict.id); });
        } catch (const std::exception&) {
        }
        return result;
    }
}
