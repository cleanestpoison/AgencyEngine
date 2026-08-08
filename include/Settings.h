#pragma once

// User-facing configuration, edited live in the SKSE Menu Framework page and
// persisted next to the DLL as Data/SKSE/Plugins/AgencyEngine.json.
//
// Char buffers rather than std::string for the text fields: ImGui::InputText
// writes into a caller-owned buffer, and keeping the storage inline in the
// struct means the UI can bind directly to the live settings object.

namespace AgencyEngine
{
    // How a generated impulse is pushed into SkyrimNet.
    enum Delivery : int
    {
        // PapyrusBridge::RecordEvent — lands in the event history of the
        // speaker and the actor they addressed, and nobody else's, but is
        // never voiced. The companion's impulse is recorded rather than said,
        // which makes this the quiet mode rather than the default one. Named
        // for SkyrimNet's RegisterPersistentEvent, which this used to call and
        // still falls back to; the config key keeps the old spelling.
        kPersistentEvent = 0,
        // DirectNarrationByUUID — hands the stage direction to the named
        // companion and gives them a speaking turn on it. This is the whole
        // point of the mod: it's what turns "she wants to go home" from a note
        // in a log into her saying so, unprompted. Needs the speaker's UUID;
        // falls back to a persistent event without one.
        kDirectNarration = 1,
    };

    // One focused question the impulse loop can ask. Each lens is a prompt file
    // that `{% extends %}`es agencyengine_impulse_base.prompt and overrides a
    // handful of prose blocks, so the JSON contract and the hard constraints
    // cannot drift between them.
    //
    // Selection happens here rather than in the template because a template
    // cannot remember anything: a fresh roll per tick produces runs, and on a
    // two-game-hour interval four aspiration turns running is most of an
    // in-game day in one register. The Director knows what it picked last.
    struct Lens
    {
        char name[64] = "";     // shown in the UI and written to the log
        char prompt[128] = "";  // resolves to prompts/<prompt>.prompt
        // Relative weight. 0 disables the lens outright, which is also how you
        // switch off a lens whose prompt depends on a mod you don't have —
        // Inja resolves unknown decorators when it parses a file, so never
        // dispatching it is the only reliable way to never parse it.
        int  weight = 0;
    };

    inline constexpr int kMaxLenses = 6;

    struct Settings
    {
        bool  enabled = true;
        // Impulse cadence, measured in *in-game* minutes. 120 = every two game
        // hours, which at the default timescale (20) is ~6 real minutes.
        float intervalGameMinutes = 120.0f;
        // How many recent SkyrimNet events to feed the prompt.
        int   maxEvents = 40;
        int   delivery = kDirectNarration;
        // After an impulse is delivered, also ask SkyrimNet for a private thought
        // from the speaker about what they just decided to raise. Costs a
        // second LLM call per spoken impulse, and buys the loop its only memory:
        // the thought lands in their event history, which this mod's own
        // prompt reads back on the next impulse.
        bool  generateThought = true;
        // Don't fire when the player is alone — this mod is about followers.
        bool  requireFollower = true;
        bool  skipInCombat = true;
        // Per-follower recent-event tail, on top of the player's.
        //
        // Thoughts per follower, because followerEventTypeFilter below restricts
        // this tail to the thought type. Ten of them all render; the tail was
        // previously thirty *events* of every type, of which the base prompt's
        // follower loop kept only the ones carrying `data.thoughts` and silently
        // dropped the rest — so after a fight it could fetch thirty and render
        // none.
        //
        // Read the two together. Unfiltered, this number is an event budget spent
        // as a thought budget and wants to be ~40 to reliably surface any. Filtered,
        // it means what it says, and 40 would be forty interior monologues per
        // follower per tick.
        int   perFollowerEvents = 10;
        // Percent chance, 0-100, that a tick is a *forced* impulse: the prompt
        // drops the silence option entirely and someone has to speak up. The
        // rest of the time the model is free to return silence, and normally
        // will, because everything else in the prompt pushes it that way.
        //
        // Injected into every impulse prompt as `forced_impulse_chance` and
        // rolled against `random` there rather than here — the roll has to be
        // suppressed mid-exchange, which the template already knows from
        // `tail_live`, and keeping both halves in one place is what stops them
        // disagreeing. Applies to every lens and to the general prompt.
        //
        // 0 never forces (quietest, purely the model's judgement); 100 forces
        // every turn, and it shows — a forced impulse on a thin day is the
        // weakest thing this prompt writes.
        int   forcedImpulseChance = 20;

        // ---- the recorded impulse, carried in her bio ----------------------
        //
        // A recorded impulse is the one that is never voiced. Carry it into her
        // next prompt verbatim by holding it in the DLL and answering a
        // decorator SkyrimNet calls while rendering *her* character bio.
        //
        // On is the whole point of the recorded delivery; off falls back to the
        // interim behaviour, where SkyrimNet is asked to generate a private
        // thought from the impulse as a hint. That costs an LLM call and
        // paraphrases the text, but it is what shipped first and is worth
        // keeping as an A/B against this.
        //
        // Nothing here can be an event: SkyrimNet stamps a proximity-and-line-of
        // -sight audience onto every event at creation, so an impulse written as
        // one is read by bystanders. Measured twice. See PendingImpulse.h.
        bool  pendingBioInjection = true;
        // How long a pending impulse stays in her bio, in *in-game* minutes.
        // 720 = half an in-game day. Past this it is dropped: something she has
        // been meaning to raise for three days is not an agenda, it is a fixture.
        float pendingTtlGameMinutes = 720.0f;
        // How often to ask the LLM whether a pending impulse has since been
        // addressed, in in-game minutes. 0 switches the check off entirely, and
        // the TTL becomes the only way one expires.
        //
        // Worth the call because the common way an agenda stops being live is
        // that the conversation covered it — which the TTL cannot see, and which
        // otherwise leaves her bio saying she has been meaning to bring up
        // something she brought up an hour ago.
        float pendingResolveGameMinutes = 180.0f;

        // ---- the ledger ---------------------------------------------------
        //
        // What each companion has already raised, rendered into the impulse
        // prompt so the loop stops proposing the same subject once it has been
        // had. The problem it solves is that the only other record — SkyrimNet's
        // event tail — is capped by count, so the evidence a subject was raised
        // drains long before the state that produced it does.
        bool  ledgerEnabled = true;
        // Slots per character, oldest evicted. Eviction is by count and not by a
        // clock on purpose: a quiet in-game week should not make a settled
        // subject raisable again, but six other beats having come and gone is a
        // fair test of "enough has changed". At most one is provisional at a
        // time, because a character can only hold one pending impulse.
        int   ledgerSlots = 6;
        // Refuse to deliver an impulse whose topic the ledger already holds.
        // A backstop, not the mechanism — the prompt is told not to repeat, and
        // this catches the times it does anyway. Costs the call that was already
        // spent, so its real value is the log line naming what was suppressed.
        bool  ledgerVeto = true;

        // Switch SkyrimNet's continuous scene mode on for the duration of a
        // fight, and back off afterwards. Off by default: it makes NPCs talk
        // autonomously *during* combat, which costs LLM calls and is a taste
        // decision, not an improvement. Only ever switches off what it switched
        // on — continuous mode the player enabled themselves is left alone.
        bool  combatContinuousMode = false;
        // How long the player must stay out of combat before it switches back
        // off. IsInCombat() drops between waves and on a brief aggro loss, so
        // without this the mode flickers — and every flicker is a HUD
        // notification from SkyrimNet, not just a log line.
        float continuousExitGraceSeconds = 10.0f;
        // ---- conversation-aware delivery -----------------------------------
        //
        // An LLM round trip takes 4-8 seconds, so checking for a conversation
        // *before* dispatching protects nothing: the answer arrives long after
        // the check, and the party may have started talking in between. The
        // check that matters is at delivery, which is instantaneous.
        //
        // So the impulse is always generated on schedule and then held until
        // the party is quiet. Generating up front rather than waiting for a gap
        // and generating then is deliberate — a gap wide enough to start a
        // request is not necessarily still open 4-8 seconds later, and chasing
        // it would loop.
        bool  deferOnConversation = true;
        // How long the party must have been silent before an impulse is spoken,
        // measured against four signals: nobody recording voice input, nothing
        // in the speech/TTS queue, this long since the last NPC audio ended,
        // and this long since anyone took a dialogue turn.
        //
        // The dialogue-turn clock is what makes the number mean anything. On
        // the audio signals alone a conversational pause — you composing a
        // line, then the LLM generating the reply — reads as total silence, and
        // 17s was routinely reached in the middle of an exchange.
        float quietSeconds = 25.0f;
        // How long a finished impulse waits for that silence before giving up.
        float maxDeferSeconds = 60.0f;
        // What happens when it gives up. A persistent event is recorded and
        // never voiced, so the companion doesn't interrupt — the topic simply
        // lands in her context and colours whatever she says next. The
        // alternative is dropping it, for anyone who would rather an impulse
        // never happened than happen silently.
        bool  degradeToPersistentEvent = true;
        // Tell the prompt how long the party has been quiet. Costs nothing once
        // the reading exists, and it is what lets the model tell "they stopped
        // talking a moment ago" from "nobody has spoken in an hour" — which
        // decides whether the last exchange is still live.
        bool  injectQuietGap = true;
        // How often to sample the signals. One static Papyrus call per
        // interval whenever either feature above is on — a held impulse has to
        // notice the moment silence arrives, and injectQuietGap needs a fresh
        // reading at dispatch, so this runs continuously rather than only while
        // something is pending.
        float quietPollSeconds = 1.0f;

        // Per-tick trace lines (snapshot contents, gate evaluation, full
        // context and response payloads). Off by default: the Director ticks
        // once a second, so this is the difference between a log you can read
        // and a log you have to grep.
        bool  debugLog = false;

        // Ask a different focused question on different ticks, instead of the
        // one general prompt. Off falls back to promptName below, unchanged —
        // which is also how you A/B a whole prompt against the lens set.
        bool  useLenses = true;
        Lens  lenses[kMaxLenses] = {
            { "Aspiration", "agencyengine_impulse_aspiration", 50 },
            { "Relationship", "agencyengine_impulse_relationship", 50 },
        };

        // Used when useLenses is off, or when every lens has been weighted to
        // zero. Kept as the general prompt it always was.
        char  promptName[128] = "agencyengine_impulse";
        // Comma-separated SkyrimNet event types; empty = all.
        char  eventTypeFilter[192] = "";
        // The same, for the per-follower tail only. Separate from the one above
        // because the two tails want different things: the player's wants
        // everything that happened, while the follower's is narrowed to thoughts
        // by the prompt regardless — so filtering here makes perFollowerEvents
        // mean that many thoughts rather than that many events that might
        // contain some.
        //
        // "npc_thoughts" is SkyrimNet's own type name for a private NPC thought
        // (its components/event_history.prompt renders that type through the
        // schema's `thoughts` field, which is the same field our follower loop
        // tests for). If a future SkyrimNet renames it, the symptom is the
        // follower thought section going empty rather than anything breaking;
        // clearing this back to "" restores the unfiltered behaviour, and
        // perFollowerEvents then wants raising.
        //
        // Empty = fall back to eventTypeFilter.
        char  followerEventTypeFilter[192] = "npc_thoughts";

        static std::filesystem::path FilePath();
        bool Load();
        bool Save() const;

        // Every field on one line, for the log. Written at startup and after
        // each save so a user-supplied log always states the configuration the
        // rest of the log was produced under.
        std::string Summary() const;
        // The enabled lenses and their weights, for the line above.
        std::string LensSummary() const;
    };

    // The single live instance. The UI mutates it on the render thread; the
    // Director copies it on its own thread. Both hold g_settingsLock.
    inline Settings   g_settings;
    inline std::mutex g_settingsLock;

    inline Settings SnapshotSettings()
    {
        std::scoped_lock lock{ g_settingsLock };
        return g_settings;
    }
}
