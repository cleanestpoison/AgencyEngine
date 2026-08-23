#pragma once

#include "PendingImpulse.h"

// Everything the Director and the UI share. Three threads touch this:
//
//   * the game's main thread   — writes GameSnapshot from an SKSE task
//   * the Director thread      — reads the snapshot, writes Status
//   * the SKSEMenuFramework render thread — reads everything
//
// so all of it lives behind one mutex, accessed through WithState().

namespace AgencyEngine
{
    struct FollowerInfo
    {
        std::string   name;
        std::uint32_t formID = 0;
    };

    // One turn of the impulse loop, kept for the UI's history list. An ask where
    // nobody had anything to raise is recorded too, with delivery "silence" —
    // an empty history and a history of twenty quiet asks mean very different
    // things when you're tuning the prompt.
    struct Impulse
    {
        std::string when;      // in-game timestamp, e.g. "day 42, 14:32"
        std::string content;   // the stage direction the LLM wrote
        std::string topic;     // the subject slug; the ledger's key, empty when silent
        std::string speaker;   // the companion who raises it; empty when silent
        std::string target;    // who they turn to
        std::string delivery;  // how it was pushed into SkyrimNet
        std::string lens;      // which question was asked; empty = single-prompt mode
        bool        ok = true;
    };

    // Carried/quiet split for one lens, over the session. The whole reason
    // cadence lives in the DLL rather than the template: a lens that never
    // produces anything is invisible in a mixed history, and "the relationship
    // question has come back empty 22 times out of 23" is the finding that
    // decides whether it needs tracked threads or just a looser prompt.
    struct LensTally
    {
        std::string name;
        // Asks that produced something she now carries, against asks that came
        // back with nobody having anything. Counted at carry, not at her saying
        // it: a cue grants the turn and SkyrimNet writes the line, so which
        // subject she actually raised is not something this mod observes.
        int         carried = 0;
        int         quiet = 0;
    };

    // One lens's clock. Every lens asks on its own cadence and there is no
    // shared tick behind them, so "when does the next thing happen" is a
    // per-lens question — this is the whole of the Director's scheduling state.
    //
    // Written by the Director thread, and by the LLM callback on a SkyrimNet
    // worker when an ask turns out to have produced a carry (which costs the
    // cooldown on top of the interval). Read by the UI. Lives in Status rather
    // than in a Director-thread static for exactly that reason: two threads
    // write it and a third reads it.
    struct LensClock
    {
        // Lens::id, or the prompt file for a lens someone wrote themselves.
        // Deliberately not the name: the name is free text, and a row renamed on
        // the Settings page must not silently rearm.
        std::string key;
        std::string name;
        // No ask before this game time. Stamped at dispatch as ask + interval,
        // and pushed out to ask + interval + cooldown if the ask carried.
        double dueGameDays = 0.0;
        // When the clock was last armed or asked. Used to notice game time
        // running backwards — which means an older save was loaded — and to
        // recompute the deadline when the cadence is moved on the settings page.
        double armedGameDays = 0.0;
        bool   asked = false;      // false = armed, never asked this session
        bool   inFlight = false;   // an ask is outstanding
        // Whether the last ask carried, and so whether the deadline above
        // includes the cooldown. Kept because the settings page can move either
        // number while the clock is running.
        bool   carried = false;
    };

    // One sample of "is anyone talking right now", from AgencyEngine_Bridge's
    // PollQuiet. All three fields come from a single Papyrus stack, so they
    // describe one instant rather than three.
    struct QuietReading
    {
        bool valid = false;  // false until the first reading arrives
        bool recording = false;
        int  speechQueue = 0;
        // 0 means "no audio has played yet" as well as "just now" — see the
        // note in AgencyEngine_Bridge.psc. Never treat 0 alone as busy.
        int  msSinceAudioEnded = 0;
        std::int64_t receivedAtMs = 0;  // steady clock, for staleness
    };

    // Refreshed by a main-thread task once per Director pass. `valid` is false
    // until a save is actually loaded (no player, no calendar).
    struct GameSnapshot
    {
        bool        valid = false;
        double      gameDays = 0.0;  // Calendar game time, in days
        std::string playerName;
        std::string location;
        bool        playerInCombat = false;
        // In-game minutes per real minute — the game's own TimeScale global.
        // 20 is vanilla; heavier modlists commonly run 6 to 10.
        //
        // Every clock in this mod is in *in-game* minutes, so this is the only
        // number that turns one into a real-world cost: the identical config
        // asks three times as often at 20 as it does at 6. Nothing in the
        // Director reads it — it is carried purely so the settings page can say
        // what each interval means in minutes the player will actually sit
        // through. Raw, including 0: a mod that freezes time sets it there, and
        // the UI has to be able to say that rather than quietly showing 20.
        float       timescale = 20.0f;
        bool        gamePaused = false;
        // The window has focus. This is a *separate* question from gamePaused:
        // only a menu pauses the simulation, so a backgrounded game keeps
        // running the clock, the followers and us. See the suspension note in
        // Director.cpp.
        bool        windowActive = true;
        // The vanilla topic-list menu is up — the player is in an ordinary
        // Skyrim conversation with somebody.
        //
        // A *third* question again, implied by neither of the two above.
        // DialogueMenu carries no kPausesGame flag, so gamePaused stays false
        // through the whole of one; the window is plainly active; and every
        // signal behind IsQuiet() is scoped to SkyrimNet's own conversations,
        // which this is not. Nothing else in this struct or in the quiet
        // reading can see it. See the vanilla-dialogue note in Director.cpp.
        bool        dialogueMenuOpen = false;
        std::vector<FollowerInfo> followers;
    };

    struct Status
    {
        bool   skyrimNetAvailable = false;
        int    skyrimNetVersion = -1;
        bool   menuFrameworkPresent = false;

        bool   inFlight = false;       // at least one ask is outstanding
        int    impulsesThisSession = 0;  // asks that produced something carried
        int    silencesThisSession = 0;  // asks where nobody had anything
        // Keyed by lens name, in first-use order. Short enough that a linear
        // scan beats a map, and the order is the one the UI wants anyway.
        std::vector<LensTally> lensTallies;
        // One per configured lens, in roster order. Rebuilt by the Director as
        // lenses are switched on and off, so a row that is no longer asked
        // leaves rather than sitting there counting down to nothing.
        std::vector<LensClock> lensClocks;

        // ---- SkyrimNet continuous scene mode, driven by combat -------------
        //
        // Written by the mod-event sink (main/VM thread) when the Papyrus helper
        // reports back, read by the Director on its own thread.
        bool continuousEnabled = false;  // last state SkyrimNet reported
        bool continuousOwned = false;    // we switched it on; we owe a switch-off
        bool continuousPending = false;  // a set was dispatched, no report yet
        // The toggle is ignored while SkyrimNet's GameMaster agent is off, and
        // there's no query for the agent — a failed acquire is how we find out.
        bool gameMasterOff = false;
        // Bumped on every *acquire* report, and only those. The Director waits
        // for it to move before deciding whether it owns the mode, so a fight
        // shorter than the round trip doesn't leave continuous mode switched
        // on. Counting switch-off reports here too would satisfy that wait with
        // the previous fight's reply — which is the one case the wait exists
        // for, since back-to-back fights are exactly when it happens.
        int  continuousAcquireReports = 0;

        // ---- conversation-aware cues --------------------------------------
        //
        // Written by the quiet sink (VM thread), read by the Director and the
        // UI. The impulse itself is carried the moment it comes back; what waits
        // for the party to stop talking is the cue that announces it.
        QuietReading quiet;
        // The player is in a vanilla conversation, or inside the settle that
        // follows one. Refreshed on the Director tick and held here for the UI
        // alone — the Director asks the predicate itself when it matters.
        //
        // Worth a field of its own rather than being folded into `quiet`: it
        // holds cues whatever `deferOnConversation` says, so the Conversation
        // panel has to be able to report it with that switch off, and reporting
        // the audio reading alone in that state would say "quiet for 40 s"
        // about a room where the player is mid-sentence with a shopkeeper.
        bool         inVanillaDialogue = false;
        // The party's own conversation is still open — somebody took a turn
        // less than conversationSettleSeconds ago. Held for the UI for the same
        // reason as the flag above: it is the state that silently holds every
        // cue, and the audio reading alone cannot report it. Reads false with
        // deferOnConversation off, because the hold does not apply then.
        bool         inConversation = false;
        bool         deliveryPending = false;   // at least one cue is waiting
        // Real seconds the oldest waiting cue has been held, for the UI.
        double       deliveryHeldSeconds = 0.0;

        std::string lastError;
        std::string lastContextJson;   // debug: what we last sent
        // Why the Director asked nothing on its most recent pass; empty
        // means nothing is blocking. Surfaced on the Status page so the answer
        // to "why is nothing happening" doesn't require opening the log.
        std::string holdReason;

        // Mirror of PendingImpulses, refreshed on the Director tick purely so
        // the UI can show them without taking that module's lock on the render
        // thread. Never the source of truth — the decorator reads the real one.
        std::vector<PendingImpulses::Entry> pendingImpulses;
        std::vector<PendingImpulses::FloorGrant> floorOwners;
        std::size_t personalMemoryRecords = 0;
        std::size_t partyMemoryRecords = 0;
        std::size_t resolutionQueuedEvidence = 0;
        std::size_t resolutionEligibleEntries = 0;
        bool resolutionBatchInFlight = false;
        std::string resolutionLastTrigger;
        std::uint64_t resolutionCallsAttempted = 0;
        std::uint64_t resolutionEntriesClassified = 0;
        std::uint64_t resolutionStaleResults = 0;
        std::uint64_t resolutionZeroCallRaises = 0;
        std::uint64_t resolutionQueueOverflow = 0;
        std::uint64_t resolutionEvidenceWatermark = 0;
        bool resolutionPollBaseline = false;
        double resolutionPollLastMilliseconds = 0.0;
        std::size_t resolutionPollTailEvents = 0;
        std::size_t resolutionPollRecoveredEvents = 0;
        std::uint64_t resolutionPollFailures = 0;

        GameSnapshot     snapshot;
        std::deque<Impulse> history;      // newest first, capped
    };

    namespace StateStore
    {
        inline constexpr std::size_t kHistoryCap = 25;

        std::mutex& Mutex();
        Status& Raw();  // only valid while holding Mutex()
    }

    // Run `fn` against the shared Status under the lock. Keep the body short —
    // this blocks the render thread.
    template <class F>
    decltype(auto) WithState(F&& fn)
    {
        std::scoped_lock lock{ StateStore::Mutex() };
        return fn(StateStore::Raw());
    }
}
