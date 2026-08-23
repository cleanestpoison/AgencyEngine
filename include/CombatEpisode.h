#pragma once

namespace AgencyEngine
{
    enum class CombatEpisodePhase
    {
        kStarted,
        kOngoing,
        kEnded,
    };

    struct CombatEpisodeSignal
    {
        CombatEpisodePhase phase = CombatEpisodePhase::kStarted;
        int                sequence = 0;
        double             elapsedSeconds = 0.0;
    };

    // Turns Skyrim's noisy IsInCombat() reading into one logical encounter.
    //
    // Observe is called on the Director's real-time cadence. Suspended samples
    // preserve the episode but advance neither its active duration nor its exit
    // grace. At most one signal is returned per observation; missed ongoing
    // intervals are deliberately coalesced instead of bursting on resume.
    class CombatEpisodeTracker
    {
    public:
        std::optional<CombatEpisodeSignal> Observe(bool         inCombat,
                                                   bool         suspended,
                                                   std::int64_t nowMs,
                                                   std::int64_t ongoingIntervalMs,
                                                   std::int64_t exitGraceMs);

        void Reset();

        [[nodiscard]] bool InEpisode() const;

    private:
        enum class State
        {
            kIdle,
            kActive,
            kExitGrace,
        };

        [[nodiscard]] CombatEpisodeSignal Signal(CombatEpisodePhase phase);
        [[nodiscard]] std::optional<CombatEpisodeSignal> EndIfGraceElapsed(std::int64_t exitGraceMs);

        State        state_ = State::kIdle;
        bool         observed_ = false;
        bool         wasSuspended_ = false;
        std::int64_t lastObservedMs_ = 0;
        std::int64_t activeElapsedMs_ = 0;
        std::int64_t graceElapsedMs_ = 0;
        std::int64_t intervalMs_ = 1;
        std::int64_t nextOngoingAtMs_ = 1;
        int          sequence_ = 0;
    };

    [[nodiscard]] std::string_view CombatEpisodePhaseName(CombatEpisodePhase phase);
}
