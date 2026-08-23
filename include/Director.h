#pragma once

#include "PendingImpulse.h"

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

    // Queue one explicitly paid manual resolution check by stable entry ID.
    // Duplicate clicks coalesce while queued.
    void RequestResolveCheck(PendingImpulses::EntryId entryId);

    // How many manual checks are waiting, for the UI's own progress line. A
    // "Check all" on a five-follower party is otherwise indistinguishable from
    // a button that did nothing.
    std::size_t PendingResolveRequests();

    // Write the game's TimeScale global, from a main-thread task.
    //
    // This is the one control on the settings page that is not ours. TimeScale
    // belongs to the game, is shared with every other mod, and is saved in the
    // save file — so it is written once, when asked, and never read back out of
    // AgencyEngine.json or reasserted at load. A mod that quietly restores a
    // game-wide setting on every startup is a mod that fights every other one
    // that touches it, and this one has no business winning that fight.
    //
    // It is here at all because every cadence in this mod is in in-game minutes
    // and this number is what they cost in real ones.
    void SetTimescale(float scale);
}
