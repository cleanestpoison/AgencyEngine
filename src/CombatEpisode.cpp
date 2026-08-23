#include "CombatEpisode.h"

namespace AgencyEngine
{
    std::optional<CombatEpisodeSignal> CombatEpisodeTracker::Observe(bool         inCombat,
                                                                     bool         suspended,
                                                                     std::int64_t nowMs,
                                                                     std::int64_t ongoingIntervalMs,
                                                                     std::int64_t exitGraceMs)
    {
        const auto deltaMs =
            observed_ && !wasSuspended_ ? std::max<std::int64_t>(0, nowMs - lastObservedMs_) : 0;
        observed_ = true;
        lastObservedMs_ = nowMs;

        if (suspended) {
            wasSuspended_ = true;
            return std::nullopt;
        }
        wasSuspended_ = false;

        const auto wantedIntervalMs = std::max<std::int64_t>(1, ongoingIntervalMs);

        switch (state_) {
        case State::kIdle:
            if (!inCombat) {
                return std::nullopt;
            }
            state_ = State::kActive;
            activeElapsedMs_ = 0;
            graceElapsedMs_ = 0;
            intervalMs_ = wantedIntervalMs;
            nextOngoingAtMs_ = intervalMs_;
            sequence_ = 0;
            return Signal(CombatEpisodePhase::kStarted);

        case State::kActive:
            if (!inCombat) {
                state_ = State::kExitGrace;
                graceElapsedMs_ = 0;
                return EndIfGraceElapsed(exitGraceMs);
            }

            activeElapsedMs_ += deltaMs;
            if (wantedIntervalMs != intervalMs_) {
                intervalMs_ = wantedIntervalMs;
                nextOngoingAtMs_ = activeElapsedMs_ + intervalMs_;
                return std::nullopt;
            }
            if (activeElapsedMs_ < nextOngoingAtMs_) {
                return std::nullopt;
            }

            sequence_ += 1;
            // Schedule from the observation that emitted, not the missed ideal
            // deadline. A stalled or suspended game therefore produces one
            // signal, never a catch-up burst.
            nextOngoingAtMs_ = activeElapsedMs_ + intervalMs_;
            return Signal(CombatEpisodePhase::kOngoing);

        case State::kExitGrace:
            if (inCombat) {
                if (wantedIntervalMs != intervalMs_) {
                    intervalMs_ = wantedIntervalMs;
                    nextOngoingAtMs_ = activeElapsedMs_ + intervalMs_;
                }
                state_ = State::kActive;
                graceElapsedMs_ = 0;
                return std::nullopt;
            }

            graceElapsedMs_ += deltaMs;
            return EndIfGraceElapsed(exitGraceMs);
        }

        return std::nullopt;
    }

    void CombatEpisodeTracker::Reset()
    {
        state_ = State::kIdle;
        observed_ = false;
        wasSuspended_ = false;
        lastObservedMs_ = 0;
        activeElapsedMs_ = 0;
        graceElapsedMs_ = 0;
        intervalMs_ = 1;
        nextOngoingAtMs_ = 1;
        sequence_ = 0;
    }

    bool CombatEpisodeTracker::InEpisode() const
    {
        return state_ != State::kIdle;
    }

    CombatEpisodeSignal CombatEpisodeTracker::Signal(CombatEpisodePhase phase)
    {
        return CombatEpisodeSignal{
            .phase = phase,
            .sequence = sequence_,
            .elapsedSeconds = static_cast<double>(activeElapsedMs_) / 1000.0,
        };
    }

    std::optional<CombatEpisodeSignal> CombatEpisodeTracker::EndIfGraceElapsed(std::int64_t exitGraceMs)
    {
        if (graceElapsedMs_ < std::max<std::int64_t>(0, exitGraceMs)) {
            return std::nullopt;
        }

        sequence_ += 1;
        const auto ended = Signal(CombatEpisodePhase::kEnded);
        state_ = State::kIdle;
        graceElapsedMs_ = 0;
        return ended;
    }

    std::string_view CombatEpisodePhaseName(CombatEpisodePhase phase)
    {
        switch (phase) {
        case CombatEpisodePhase::kStarted:
            return "started"sv;
        case CombatEpisodePhase::kOngoing:
            return "ongoing"sv;
        case CombatEpisodePhase::kEnded:
            return "ended"sv;
        }
        return "unknown"sv;
    }
}
