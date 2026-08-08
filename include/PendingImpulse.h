#pragma once

// A companion's *recorded* impulse — the one that is never voiced — carried
// into her next prompt verbatim, and into nobody else's.
//
// WHY THIS EXISTS, AND WHY IT IS NOT AN EVENT
//
// SkyrimNet stamps an audience onto every event at creation, from proximity and
// line of sight, and nothing in either API takes an audience argument. Measured
// twice: an impulse written as our own agencyengine_event type was read by a
// bystanding follower's dialogue prompt, and rewriting the same impulse as
// SkyrimNet's own npc_thoughts type leaked identically. The audience follows the
// creation path, not the event type — anything written through the Papyrus
// natives is public to the scene. See PapyrusBridge.h for the full finding.
//
// So the impulse never becomes an event at all. It is held here, in the DLL, and
// reaches exactly one prompt through a decorator SkyrimNet calls on us while it
// renders that NPC's character bio:
//
//   statics/.../prompts/submodules/character_bio/7200_pending_impulse.prompt
//
// which is guarded on render_mode so it renders into her own prompt and not into
// the scene-context summary every bystander gets. That guard is load-bearing;
// the prompt file says so at length.
//
// No LLM call delivers it, so the composed text survives word for word instead
// of being paraphrased by a generated thought.
//
// THREADING. Three threads touch this and it has its own mutex, deliberately not
// the Status one: the decorator callback runs on SkyrimNet's thread in the
// middle of rendering a prompt, and must not queue behind the UI's whole-Status
// copy. Every function here is safe to call from anywhere; only the decorator
// path (Get) is hot, and it copies one string under the lock and returns.

namespace AgencyEngine::PendingImpulses
{
    // The decorator SkyrimNet calls to reach Get(). Registered from
    // SkyrimNetAPI::RegisterDecorator on kDataLoaded; referenced by name in the
    // bio prompt, so the two must be changed together.
    inline constexpr auto kDecoratorName = "agencyengine_pending_impulse";

    // The companion piece: returns "carried" / "spoken" / "" so the same bio
    // block can say "you have not said this" or "you said this and it went
    // unanswered" without a second decorator call per wording.
    inline constexpr auto kStateDecoratorName = "agencyengine_pending_state";

    struct Entry
    {
        std::uint32_t formID = 0;
        std::string   speakerName;  // as captured, for the load-order check below
        std::string   targetName;   // who she meant to raise it with
        std::string   text;         // the stage direction, verbatim
        std::string   topic;        // short slug; the key the ledger suppresses on
        std::string   lens;
        double        createdGameDays = 0.0;
        // She was handed a speaking turn on this and said it. The entry stays
        // alive afterwards rather than being dropped, because "she said it" and
        // "it got anywhere" are different questions and only the first one is
        // ours to answer. The bio renders a different block for a spoken entry,
        // and the resolution check asks a different question about it.
        bool          spoken = false;
        // When she said it. The TTL runs from here once spoken, not from
        // createdGameDays — otherwise a beat carried most of its life before
        // being voiced would expire minutes later, and how long the spoken
        // phase lasts would be whatever happened to be left over.
        double        spokenGameDays = 0.0;
        // When the resolution check last asked whether this is still live.
        // Equal to createdGameDays until the first check.
        double        lastCheckGameDays = 0.0;
        // Restored from disk and not yet matched against the game. FormIDs are
        // load-order dependent, so a persisted one can name a different actor
        // after the user installs a mod; the Director resolves these on the main
        // thread and drops any whose name no longer matches.
        bool          unverified = false;
    };

    // Record `entry` as `formID`'s pending impulse, replacing any previous one.
    // One per actor: a second agenda does not stack, it supersedes, because two
    // things she has been meaning to raise read as a list rather than a person.
    void Set(Entry entry);

    // The decorator path. Returns the pending text, or "" when there is none —
    // the prompt guards on empty. Never blocks on anything but its own lock.
    std::string Get(std::uint32_t formID);

    // What clearing an entry does to the ledger slot it left behind. Passed
    // explicitly rather than sniffed out of `reason`, which is free text written
    // for a human reading the log.
    enum class Disposition
    {
        // The subject never got an answer, so it goes back into circulation the
        // moment the entry dies. The default, because every way of clearing an
        // entry *except* an explicit resolved verdict means exactly that.
        Withdraw,
        // The resolution check judged the subject dealt with. The slot stays and
        // keeps suppressing until six other beats displace it.
        Confirm,
    };

    // Drops `formID`'s pending impulse and logs why. `reason` is one of "ttl",
    // "resolved", "spoken", "stale" — a bio line that silently persists reads as
    // fixation and is near-impossible to diagnose after the fact, so every clear
    // names itself. Returns false when there was nothing to clear.
    bool Clear(std::uint32_t formID, std::string_view reason, Disposition disposition = Disposition::Withdraw);

    // Her state, for the bio decorator: "" when nothing is pending, "carried"
    // when she has not said it, "spoken" when she has. A separate decorator from
    // the text one so the prompt can branch its wording without parsing a
    // sentinel out of the impulse itself.
    std::string State(std::uint32_t formID);

    std::vector<Entry> Snapshot();
    std::size_t        Count();

    // ---- the ledger -------------------------------------------------------
    //
    // What each companion has already raised, so the impulse prompt stops
    // proposing it again. This exists because the only other record is
    // SkyrimNet's event tail, which is capped by *count* and drains at whatever
    // rate events happen — so the evidence that a subject was raised ages out
    // long before the state that produced it does, and the loop reaches for it
    // again in good faith. This one is ours and does not decay.
    //
    // Eviction is by count, not by a clock. Six per character; the seventh
    // pushes out the oldest. A quiet in-game week should not make a settled
    // subject raisable again, but six other beats having come and gone is a
    // decent test of "enough has changed".
    //
    // A slot is written *provisional* at dispatch and decided when the entry
    // that owns it dies (see Disposition). Provisional slots suppress exactly
    // like confirmed ones — the immediate next-tick repeat is what they are for
    // — but they are withdrawn rather than kept if the subject turns out never
    // to have been answered. At most one slot per character is provisional at a
    // time, because Set() allows only one pending entry per actor, so the cap
    // can never evict a slot whose entry is still live.

    struct LedgerSlot
    {
        std::uint32_t formID = 0;
        std::string   speakerName;
        std::string   topic;
        bool          provisional = true;
    };

    // Records `topic` against `formID` as provisional, evicting that
    // character's oldest slot past `cap`. A repeat of a topic already held moves
    // it to newest rather than taking a second slot.
    void LedgerRecord(std::uint32_t formID, std::string speakerName, std::string topic, std::size_t cap);

    // Decide a provisional slot. Confirm keeps it; Withdraw drops it. Both are
    // no-ops on a topic that is not held, so a delivery that never got a topic
    // out of the model costs nothing here.
    void LedgerDecide(std::uint32_t formID, std::string_view topic, Disposition disposition);

    // What `formID` has already raised, newest last — the lines the impulse
    // prompt renders. Empty when there is nothing.
    std::vector<std::string> LedgerTopics(std::uint32_t formID);

    // Whether `topic` is currently suppressed for `formID`. Compared loosely
    // (case and surrounding punctuation ignored), because the model writes the
    // slug freshly each time and "the coin split" and "The coin split." are the
    // same subject.
    bool LedgerSuppresses(std::uint32_t formID, std::string_view topic);

    // Slots per character, republished by the Director every tick. Clear() needs
    // it — a subject confirmed settled while she was still carrying it unsaid has
    // no slot yet and gets one there — and Clear is called from places that have
    // no access to Settings.
    void SetLedgerCap(std::size_t cap);

    std::vector<LedgerSlot> LedgerSnapshot();

    // Drops everything created more than `ttlGameMinutes` of *game* time ago.
    // Game time, not real time: the cadence this whole mod runs on is in-game
    // hours, and a TTL in real minutes would expire during a loading screen and
    // survive a night spent sleeping.
    void ExpireOlderThan(double nowGameDays, float ttlGameMinutes);

    // The entry whose resolution check is due, if any, oldest check first. Call
    // MarkChecked as soon as the request is dispatched — not when it answers, or
    // an unanswered check retries every tick.
    std::optional<Entry> NextDueForCheck(double nowGameDays, float intervalGameMinutes);
    void                 MarkChecked(std::uint32_t formID, double nowGameDays);

    // Entries restored from disk that have not been matched against the game
    // yet. Marks them verified as it hands them over, so this returns each one
    // exactly once; the caller is expected to look each FormID up on the main
    // thread and Clear the ones whose actor is gone or renamed.
    std::vector<Entry> TakeUnverified();

    // Forget everything in memory and forget which save we loaded from, so the
    // next SyncPersistence reloads. Called on kNewGame / kPostLoadGame.
    void Reset();

    // Load-on-first-sight, save-when-dirty, both keyed off SkyrimNet's save ID.
    //
    // Called every Director tick with the current save ID (empty when no save is
    // loaded, which is a no-op). The first tick after a load sees an ID we have
    // not loaded for and reads it off disk, dropping anything already past its
    // TTL against the current game time. Doing it this way rather than from the
    // kPostLoadGame message sidesteps the ordering question entirely — we simply
    // never act until SkyrimNet can tell us which save we are in.
    void SyncPersistence(const std::string& saveId, double nowGameDays, float ttlGameMinutes);

    // Data/SKSE/Plugins/AgencyEngine_Pending.json — a sidecar keyed by save ID,
    // alongside AgencyEngine.json. Deliberately not SKSE co-save serialization:
    // a sidecar matches how Settings already works and keeps the DLL free of
    // co-save versioning for something this small.
    std::filesystem::path FilePath();
}
