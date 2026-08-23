#include "SkyrimNetEvent.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace AgencyEngine::SkyrimNetAPI
{
    namespace
    {
        std::uint64_t JsonIdentity(const nlohmann::json& value)
        {
            if (value.is_number_unsigned()) {
                return value.get<std::uint64_t>();
            }
            if (value.is_number_integer()) {
                const auto signedValue = value.get<std::int64_t>();
                return signedValue > 0 ? static_cast<std::uint64_t>(signedValue) : 0;
            }
            if (value.is_string()) {
                try {
                    return std::stoull(value.get<std::string>(), nullptr, 0);
                } catch (const std::exception&) {
                    return 0;
                }
            }
            return 0;
        }

        std::string SourceIdentity(const nlohmann::json& value)
        {
            for (const auto* key : { "id", "eventId", "event_id", "sourceId" }) {
                if (value.contains(key)) {
                    return value[key].is_string() ? value[key].get<std::string>() : value[key].dump();
                }
            }
            return {};
        }

        std::string EventText(const nlohmann::json& value)
        {
            const auto copyText = [](const nlohmann::json& object) {
                for (const auto* key : { "text", "dialogue", "message", "transcription" }) {
                    if (object.contains(key) && object[key].is_string()) {
                        return object[key].get<std::string>();
                    }
                }
                return std::string{};
            };

            auto text = copyText(value);
            if (!text.empty() || !value.contains("data")) {
                return text;
            }
            const auto& data = value["data"];
            if (data.is_object()) {
                return copyText(data);
            }
            if (!data.is_string()) {
                return {};
            }
            const auto encoded = data.get<std::string>();
            const auto nested = nlohmann::json::parse(encoded, nullptr, false);
            if (nested.is_object()) {
                return copyText(nested);
            }
            if (nested.is_string()) {
                return nested.get<std::string>();
            }
            return nested.is_discarded() ? encoded : std::string{};
        }

        std::uint64_t FirstIdentity(const nlohmann::json& value,
                                    std::initializer_list<const char*> keys)
        {
            for (const auto* key : keys) {
                if (value.contains(key)) {
                    if (const auto identity = JsonIdentity(value[key]); identity != 0) {
                        return identity;
                    }
                }
            }
            return 0;
        }
    }

    RawDialogueEvent ParseEventCallbackPayload(std::string_view payload, std::string type,
                                               std::int64_t arrivalMs)
    {
        RawDialogueEvent event;
        event.type = std::move(type);
        event.arrivalMs = arrivalMs;
        if (payload.empty()) {
            return event;
        }
        try {
            const auto value = nlohmann::json::parse(payload);
            if (!value.is_object()) {
                return event;
            }
            event.sourceId = SourceIdentity(value);
            // Pending entries and floor grants use SkyrimNet UUIDs. Retain
            // FormIDs only as a fallback for older callback envelopes.
            event.actorId = FirstIdentity(
                value, { "originatingActorUUID", "originatingActor", "originatingActorFormId",
                         "actorFormId", "actor", "actorId" });
            event.targetId = FirstIdentity(
                value, { "targetActorUUID", "targetActor", "targetActorFormId", "targetFormId",
                         "target", "targetId" });
            if (value.contains("type") && value["type"].is_string()) {
                event.type = value["type"].get<std::string>();
            }
            event.text = EventText(value);
        } catch (const std::exception&) {
        }
        return event;
    }

    RecentEventRecovery::RecentEventRecovery(std::size_t sourceCapacity) :
        sourceCapacity_(std::max<std::size_t>(sourceCapacity, 1))
    {}

    void RecentEventRecovery::BeginSave(std::string saveId)
    {
        saveId_ = std::move(saveId);
        baselineEstablished_ = false;
        sourceIds_.clear();
        sourceOrder_.clear();
    }

    void RecentEventRecovery::Remember(std::string sourceId)
    {
        if (sourceId.empty() || !sourceIds_.insert(sourceId).second) {
            return;
        }
        sourceOrder_.push_back(std::move(sourceId));
        while (sourceOrder_.size() > sourceCapacity_) {
            sourceIds_.erase(sourceOrder_.front());
            sourceOrder_.pop_front();
        }
    }

    void RecentEventRecovery::ObserveCallbacks(std::span<const RawDialogueEvent> events)
    {
        for (const auto& event : events) {
            Remember(event.sourceId);
        }
    }

    RecentEventPoll RecentEventRecovery::Poll(std::string_view payload, std::int64_t steadyNowMs,
                                              std::int64_t unixNowMs)
    {
        RecentEventPoll result;
        const auto value = nlohmann::json::parse(payload, nullptr, false);
        if (!value.is_array()) {
            return result;
        }
        result.valid = true;
        result.tailSize = value.size();

        std::vector<RawDialogueEvent> parsed;
        parsed.reserve(value.size());
        for (const auto& item : value) {
            if (!item.is_object()) {
                continue;
            }
            RawDialogueEvent event;
            event.sourceId = SourceIdentity(item);
            if (event.sourceId.empty()) {
                continue;
            }
            event.actorId = FirstIdentity(
                item, { "originatingActor", "originatingActorUUID", "originatingActorFormId",
                        "actor", "actorId" });
            event.targetId = FirstIdentity(
                item, { "targetActor", "targetActorUUID", "targetActorFormId", "target", "targetId" });
            if (item.contains("type") && item["type"].is_string()) {
                event.type = item["type"].get<std::string>();
            }
            event.text = EventText(item);
            event.arrivalMs = steadyNowMs;
            if (item.contains("localTime") && item["localTime"].is_number()) {
                const auto eventUnixMs =
                    static_cast<std::int64_t>(std::llround(item["localTime"].get<double>() * 1000.0));
                const auto ageMs = std::max<std::int64_t>(unixNowMs - eventUnixMs, 0);
                event.arrivalMs = steadyNowMs - ageMs;
            }
            parsed.push_back(std::move(event));
        }
        std::ranges::stable_sort(parsed, {}, &RawDialogueEvent::arrivalMs);

        if (!baselineEstablished_) {
            for (const auto& event : parsed) {
                Remember(event.sourceId);
            }
            baselineEstablished_ = true;
            result.establishedBaseline = true;
            return result;
        }

        result.events.reserve(parsed.size());
        for (auto& event : parsed) {
            if (sourceIds_.contains(event.sourceId)) {
                continue;
            }
            Remember(event.sourceId);
            result.events.push_back(std::move(event));
        }
        return result;
    }
}
