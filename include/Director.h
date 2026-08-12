#pragma once

// The impulse loop. A single background thread passes once a second, asks the
// main thread for a fresh snapshot of the world, and checks each lens's own
// in-game clock. For every lens that has come due it assembles a context payload
// from SkyrimNet's recent-event tail and asks the LLM for one short narrative
// impulse about the player and their followers; most passes ask nothing at all.
// The result is written back into SkyrimNet (see PapyrusBridge).
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

    // "Ask now" from the UI: asks `lensKey` — Lens::id, or the prompt file for a
    // hand-authored row — on the next pass, regardless of its clock and of the
    // requireFollower / skipInCombat gates. Empty asks whichever lens is nearest
    // due, which is what the Status page's single button means.
    //
    // A manual ask stamps the clock exactly like a scheduled one, so it spends
    // that lens's next natural ask (and the cooldown too, if it carries). The
    // alternative — a free probe — would leave the log describing a cadence the
    // lens was not actually keeping.
    //
    // Deduplicated per lens: pressing one row's button twice costs one call, and
    // pressing two rows' costs two, because they are different questions.
    void RequestFireNow(const std::string& lensKey = {});

    // Rearm every lens clock from the current game time.
    void ResetTimer();

    // "Check this one now" from the UI: runs the resolution check against one
    // pending impulse on the next pass, ignoring both the in-game cadence and a
    // cadence of 0 (the check switched off). Queued rather than run inline —
    // the UI call arrives on the render thread, and the check needs the
    // Director's own thread to reach SkyrimNet's DB without stuttering a frame.
    //
    // Named by actor *and* lens, because a companion can be carrying one impulse
    // from each and they are separate questions with separate answers.
    //
    // Requests are deduplicated, so pressing the button twice costs one call.
    // They still queue behind an in-flight check, and behind each other: the
    // whole point of one-at-a-time is that a large party cannot fan out into a
    // dozen simultaneous LLM requests.
    void RequestResolveCheck(std::uint32_t formID, const std::string& lens);

    // How many manual checks are waiting, for the UI's own progress line. A
    // "Check all" on a five-follower party is otherwise indistinguishable from
    // a button that did nothing.
    std::size_t PendingResolveRequests();
}
