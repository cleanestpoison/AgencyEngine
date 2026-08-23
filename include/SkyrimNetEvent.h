#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace AgencyEngine::SkyrimNetAPI
{
    struct RawDialogueEvent
    {
        std::string sourceId;
        std::uint64_t actorId = 0;
        std::uint64_t targetId = 0;
        std::string type;
        std::string text;
        std::int64_t arrivalMs = 0;
    };

    // Pure decoder for PublicRegisterEventCallback's provider-owned JSON
    // envelope. Kept outside SkyrimNetAPI.cpp so callback fixtures can exercise
    // the real boundary without loading SkyrimNet.dll.
    RawDialogueEvent ParseEventCallbackPayload(std::string_view payload, std::string type,
                                               std::int64_t arrivalMs);

    struct RecentEventPoll
    {
        std::vector<RawDialogueEvent> events;
        std::size_t tailSize = 0;
        bool valid = false;
        bool establishedBaseline = false;
    };

    // Recovery cursor for PublicGetRecentEvents. The first valid tail after
    // every BeginSave is a baseline; later polls return each source ID once.
    class RecentEventRecovery
    {
    public:
        explicit RecentEventRecovery(std::size_t sourceCapacity = 2048);

        void BeginSave(std::string saveId);
        void ObserveCallbacks(std::span<const RawDialogueEvent> events);
        RecentEventPoll Poll(std::string_view payload, std::int64_t steadyNowMs,
                             std::int64_t unixNowMs);

    private:
        void Remember(std::string sourceId);

        std::size_t sourceCapacity_;
        std::string saveId_;
        bool baselineEstablished_ = false;
        std::set<std::string> sourceIds_;
        std::deque<std::string> sourceOrder_;
    };
}
