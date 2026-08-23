#include "CombatEpisode.h"

#include <cstdio>

namespace
{
    using AgencyEngine::CombatEpisodePhase;
    using AgencyEngine::CombatEpisodeSignal;
    using AgencyEngine::CombatEpisodeTracker;

    int g_checks = 0;
    int g_failures = 0;

    void Check(bool condition, const char* what, const char* file, int line)
    {
        g_checks += 1;
        if (!condition) {
            g_failures += 1;
            std::printf("  FAILED: %s\n    at %s:%d\n", what, file, line);
        }
    }

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)

    void CheckSignal(const std::optional<CombatEpisodeSignal>& signal,
                     CombatEpisodePhase                        phase,
                     int                                       sequence,
                     double                                    elapsedSeconds)
    {
        CHECK(signal.has_value());
        if (!signal) {
            return;
        }
        CHECK(signal->phase == phase);
        CHECK(signal->sequence == sequence);
        CHECK(signal->elapsedSeconds == elapsedSeconds);
    }

    void EmitsOneLifecycleAtTheConfiguredCadence()
    {
        std::printf("- one lifecycle emits start, ongoing, and end\n");
        CombatEpisodeTracker tracker;

        CheckSignal(tracker.Observe(true, false, 1'000, 15'000, 10'000), CombatEpisodePhase::kStarted, 0, 0.0);
        CHECK(!tracker.Observe(true, false, 15'999, 15'000, 10'000));
        CheckSignal(tracker.Observe(true, false, 16'000, 15'000, 10'000), CombatEpisodePhase::kOngoing, 1, 15.0);
        CHECK(!tracker.Observe(false, false, 17'000, 15'000, 10'000));
        CHECK(tracker.InEpisode());
        CHECK(!tracker.Observe(false, false, 26'999, 15'000, 10'000));
        CheckSignal(tracker.Observe(false, false, 27'000, 15'000, 10'000), CombatEpisodePhase::kEnded, 2, 15.0);
        CHECK(!tracker.InEpisode());
    }

    void BriefCombatDropsRemainOneEpisode()
    {
        std::printf("- a brief IsInCombat drop remains one episode\n");
        CombatEpisodeTracker tracker;

        CheckSignal(tracker.Observe(true, false, 1'000, 15'000, 10'000), CombatEpisodePhase::kStarted, 0, 0.0);
        CHECK(!tracker.Observe(true, false, 11'000, 15'000, 10'000));
        CHECK(!tracker.Observe(false, false, 12'000, 15'000, 10'000));
        CHECK(!tracker.Observe(false, false, 17'000, 15'000, 10'000));
        CHECK(!tracker.Observe(true, false, 18'000, 15'000, 10'000));
        CheckSignal(tracker.Observe(true, false, 23'000, 15'000, 10'000), CombatEpisodePhase::kOngoing, 1, 15.0);
    }

    void SuspensionAdvancesNeitherCadenceNorGrace()
    {
        std::printf("- suspension advances neither cadence nor exit grace\n");
        CombatEpisodeTracker tracker;

        CheckSignal(tracker.Observe(true, false, 1'000, 15'000, 10'000), CombatEpisodePhase::kStarted, 0, 0.0);
        CHECK(!tracker.Observe(true, false, 11'000, 15'000, 10'000));
        CHECK(!tracker.Observe(true, true, 12'000, 15'000, 10'000));
        CHECK(!tracker.Observe(true, true, 112'000, 15'000, 10'000));
        CHECK(!tracker.Observe(true, false, 113'000, 15'000, 10'000));
        CheckSignal(tracker.Observe(true, false, 118'000, 15'000, 10'000), CombatEpisodePhase::kOngoing, 1, 15.0);

        CHECK(!tracker.Observe(false, false, 119'000, 15'000, 10'000));
        CHECK(!tracker.Observe(false, true, 124'000, 15'000, 10'000));
        CHECK(!tracker.Observe(false, true, 224'000, 15'000, 10'000));
        CHECK(!tracker.Observe(false, false, 225'000, 15'000, 10'000));
        CHECK(!tracker.Observe(false, false, 234'999, 15'000, 10'000));
        CheckSignal(tracker.Observe(false, false, 235'000, 15'000, 10'000), CombatEpisodePhase::kEnded, 2, 15.0);
    }

    void StallsCoalesceAndLiveIntervalChangesRearm()
    {
        std::printf("- stalls coalesce and an interval edit rearms from now\n");
        CombatEpisodeTracker tracker;

        CheckSignal(tracker.Observe(true, false, 1'000, 15'000, 10'000), CombatEpisodePhase::kStarted, 0, 0.0);
        CheckSignal(tracker.Observe(true, false, 61'000, 15'000, 10'000), CombatEpisodePhase::kOngoing, 1, 60.0);
        CHECK(!tracker.Observe(true, false, 62'000, 5'000, 10'000));
        CHECK(!tracker.Observe(true, false, 66'999, 5'000, 10'000));
        CheckSignal(tracker.Observe(true, false, 67'000, 5'000, 10'000), CombatEpisodePhase::kOngoing, 2, 66.0);
    }

    void ResetCancelsTheOldSaveWithoutAnEndSignal()
    {
        std::printf("- reset cancels the old save without an end signal\n");
        CombatEpisodeTracker tracker;

        CheckSignal(tracker.Observe(true, false, 1'000, 15'000, 10'000), CombatEpisodePhase::kStarted, 0, 0.0);
        tracker.Reset();
        CHECK(!tracker.InEpisode());
        CHECK(!tracker.Observe(false, false, 20'000, 15'000, 10'000));
        CheckSignal(tracker.Observe(true, false, 21'000, 15'000, 10'000), CombatEpisodePhase::kStarted, 0, 0.0);
    }

    void ZeroGraceEndsOnTheFirstOutOfCombatSample()
    {
        std::printf("- zero exit grace ends on the first out-of-combat sample\n");
        CombatEpisodeTracker tracker;

        CheckSignal(tracker.Observe(true, false, 1'000, 15'000, 0), CombatEpisodePhase::kStarted, 0, 0.0);
        CheckSignal(tracker.Observe(false, false, 2'000, 15'000, 0), CombatEpisodePhase::kEnded, 1, 0.0);
        CHECK(!tracker.InEpisode());
    }
}

int main()
{
    std::printf("AgencyEngine combat episode tests\n");

    EmitsOneLifecycleAtTheConfiguredCadence();
    BriefCombatDropsRemainOneEpisode();
    SuspensionAdvancesNeitherCadenceNorGrace();
    StallsCoalesceAndLiveIntervalChangesRearm();
    ResetCancelsTheOldSaveWithoutAnEndSignal();
    ZeroGraceEndsOnTheFirstOutOfCombatSample();

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
