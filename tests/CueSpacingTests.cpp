#include "CueSpacing.h"

#include <cstdio>

namespace
{
    using AgencyEngine::CueSpacingGate;

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

    void SerializesAttemptsAcrossTheParty()
    {
        std::printf("- one attempt excludes every other cue for sixty seconds\n");
        CueSpacingGate gate;

        CHECK(gate.TryBeginAttempt(1'000));
        CHECK(!gate.TryBeginAttempt(1'000));
        CHECK(!gate.TryBeginAttempt(60'999));
        CHECK(gate.TryBeginAttempt(61'000));
    }

    void EveryAttemptOccupiesSpacing()
    {
        std::printf("- a failed dispatch attempt still occupies spacing\n");
        CueSpacingGate gate;

        CHECK(gate.TryBeginAttempt(5'000));
        CHECK(!gate.TryBeginAttempt(64'999));
        CHECK(gate.TryBeginAttempt(65'000));
    }

    void ReportsOnlyTheBlockedPartOfAPumpInterval()
    {
        std::printf("- defer accounting excludes exactly the spacing overlap\n");
        CueSpacingGate gate;

        CHECK(gate.TryBeginAttempt(10'000));
        CHECK(gate.BlockedDurationMs(5'000, 15'000) == 5'000);
        CHECK(gate.BlockedDurationMs(69'000, 71'000) == 1'000);
        CHECK(gate.BlockedDurationMs(70'000, 71'000) == 0);
    }

    void SuspensionDoesNotAdvanceSpacing()
    {
        std::printf("- suspended time does not advance spacing\n");
        CueSpacingGate gate;

        CHECK(gate.TryBeginAttempt(10'000));
        gate.Pause(90'000);
        CHECK(gate.RemainingMs(159'999) == 1);
        CHECK(!gate.TryBeginAttempt(159'999));
        CHECK(gate.TryBeginAttempt(160'000));
    }

    void ResetDropsThePreviousSavesSpacing()
    {
        std::printf("- reset drops spacing owned by the previous save\n");
        CueSpacingGate gate;

        CHECK(gate.TryBeginAttempt(20'000));
        gate.Reset();
        CHECK(gate.RemainingMs(20'001) == 0);
        CHECK(gate.TryBeginAttempt(20'001));
    }
}

int main()
{
    std::printf("AgencyEngine cue spacing tests\n");

    SerializesAttemptsAcrossTheParty();
    EveryAttemptOccupiesSpacing();
    ReportsOnlyTheBlockedPartOfAPumpInterval();
    SuspensionDoesNotAdvanceSpacing();
    ResetDropsThePreviousSavesSpacing();

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
