#pragma once

// The impulse loop. A single background thread wakes once a second, asks the main
// thread for a fresh snapshot of the world, and — when enough *in-game* time
// has passed since the last impulse — assembles a context payload from SkyrimNet's
// recent-event tail and asks the LLM for one short narrative impulse about the
// player and their followers. The result is written back into SkyrimNet as an
// event (see PapyrusBridge).
//
// Thread discipline:
//   * game reads (calendar, followers, combat state) happen only inside the
//     main-thread task the loop posts;
//   * SkyrimNet DB queries happen on the Director thread (they hit SQLite and
//     would stutter the frame);
//   * the Papyrus write happens back on the main thread.

namespace AgencyEngine::Director
{
    void Start();
    void Stop();

    // "Generate now" from the UI: asks on the next pass regardless of the
    // interval and the requireFollower / skipInCombat gates.
    void RequestFireNow();

    // Restart the interval countdown from the current game time.
    void ResetTimer();

    // "Check this one now" from the UI: runs the resolution check against a
    // pending impulse on the next pass, ignoring both the in-game cadence and a
    // cadence of 0 (the check switched off). Queued rather than run inline —
    // the UI call arrives on the render thread, and the check needs the
    // Director's own thread to reach SkyrimNet's DB without stuttering a frame.
    //
    // Requests are deduplicated, so pressing the button twice costs one call.
    // They still queue behind an in-flight check, and behind each other: the
    // whole point of one-at-a-time is that a large party cannot fan out into a
    // dozen simultaneous LLM requests.
    void RequestResolveCheck(std::uint32_t formID);

    // How many manual checks are waiting, for the UI's own progress line. A
    // "Check all" on a five-follower party is otherwise indistinguishable from
    // a button that did nothing.
    std::size_t PendingResolveRequests();
}
