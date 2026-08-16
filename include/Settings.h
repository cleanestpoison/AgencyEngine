#pragma once

// User-facing configuration, edited live in the SKSE Menu Framework page and
// persisted next to the DLL as Data/SKSE/Plugins/AgencyEngine.json.
//
// Char buffers rather than std::string for the text fields: ImGui::InputText
// writes into a caller-owned buffer, and keeping the storage inline in the
// struct means the UI can bind directly to the live settings object.

namespace AgencyEngine
{
    // There is no delivery *mode* any more. An impulse is always carried — held
    // in the DLL and rendered into her own character bio — and the only
    // narration left is the cue, which grants her a speaking turn and names no
    // subject. See docs/adr/0004 and the `cues` setting below.

    // One focused question the impulse loop can ask. Each lens is a prompt file
    // that `{% extends %}`es agencyengine_impulse_base.prompt and overrides a
    // handful of prose blocks, so the JSON contract and the hard constraints
    // cannot drift between them.
    //
    // Every lens runs on its own clock. There is no shared tick and no draw
    // between them: asking two questions is not worse than asking one, it is
    // just two asks, and lenses whose material accumulates at different rates
    // have no business sharing one cadence knob. See docs/adr/0003.
    //
    // A lens is *content*, not configuration. Its name, its prompt file and its
    // proposal semantics are properties of a file that ships in the archive
    // beside the DLL — nobody can author them by typing into the settings page,
    // and a prompt name that doesn't resolve costs the whole impulse silently.
    // Only the enable switch, the two cadence numbers and `ledgerSlots` are
    // preferences. So the shipped roster is kBuiltinLenses below and the config
    // file stores overrides against it, keyed by `id`. A lens added in a later
    // release then appears on its own, at its shipped cadence, in a config that
    // already exists.
    struct Lens
    {
        // Stable key, used only to match a config override to a shipped lens.
        // Never displayed, never edited, and deliberately not `name` — the name
        // is free text a user can rename, and a rename must cost them their
        // tuning no more than it costs them the prompt file. Empty marks a
        // hand-authored lens, which has no shipped row to be an override of.
        char id[32] = "";
        char name[64] = "";     // shown in the UI and written to the log
        char prompt[128] = "";  // resolves to prompts/<prompt>.prompt
        // Is this lens asked at all? Off is how you switch off a lens whose
        // prompt depends on a mod you don't have — Inja resolves unknown
        // decorators when it *parses* a file, so never dispatching it is the
        // only reliable way to never parse it.
        //
        // This replaced `weight`, which existed to decide which lens won a turn
        // when they competed for one. They no longer compete, so the only job
        // weight had left was *off* — and a bool says that without implying the
        // other ninety-nine values mean anything.
        bool  enabled = false;
        // In-game minutes between asks. A quiet ask costs one interval; an ask
        // that produces a carried impulse costs interval + cooldown.
        //
        // This is both the chattiness knob and the cost knob: a quiet ask is
        // still an LLM call, and there is no tick behind it throttling anything,
        // so lengthening the interval is the only way to make a lens cheaper.
        float intervalGameMinutes = 120.0f;
        // In-game minutes of silence *on top of* the interval once this lens's
        // question has landed as a carried impulse.
        //
        // Keyed on carry rather than on her speaking it: speech can lag a carry
        // indefinitely, and a lens gated on speech could re-ask about a subject
        // she is already carrying. This is what makes the anti-nag structural —
        // a lens cannot re-raise inside its cooldown because it is never asked,
        // not because the prompt asks it nicely not to.
        float cooldownGameMinutes = 480.0f;
        // Does this lens produce *proposals* rather than *topics*?
        //
        // A topic is met when someone answers it — agreeing, refusing, arguing
        // it out. A proposal ("come drink", "spar with me") is not: agreement
        // alone means nothing has happened yet, and the resolution check has to
        // treat "sure" as the deferral it is or the impulse is buried at the
        // exact moment the player says yes.
        //
        // Declared here rather than inferred from `name`, which is free text the
        // user can edit — a rename would silently revert a lens to topic
        // semantics with no error and no log line. False by default, so a lens
        // configured before this existed keeps behaving exactly as it did.
        bool proposal = false;
        // Ledger slots for this lens, or 0 to use the global count.
        //
        // Per lens because the rings are per lens and the right size differs by
        // an order of magnitude: topics are unlimited and want a long memory,
        // while proposals come from a closed vocabulary of under ten subjects —
        // a six-slot ring holds nearly all of it and vetoes the lens into
        // silence. Three gives a natural four-proposal rotation instead.
        int  ledgerSlots = 0;
    };

    inline constexpr int kMaxLenses = 6;

    // The lens roster this build ships, and the source of every default in the
    // table below. Changing a cadence here changes it for every install that has
    // not moved that control — see Settings::Save, which writes only what differs
    // from the shipped value, so a default is not frozen by the act of opening
    // the settings page and saving something unrelated.
    //
    // Cadence, in game hours (interval / cooldown): Aspiration 2/8 is the
    // workhorse and asks often. Relationship's material — standing that has gone
    // unsaid — accumulates over in-game days, so a long interval buys fewer quiet
    // asks against the same data rather than more answers. Activity's danger is
    // repeat-proposing ("spar with me" again tonight), so it takes much the
    // longest cooldown of the three. See docs/adr/0003.
    //
    // Adding a row: give it an id that will never change (the ledger and the
    // config both key on it), and keep the count under kMaxLenses — the trailing
    // slots are what a user's own lenses occupy.
    inline constexpr Lens kBuiltinLenses[] = {
        { "aspiration", "Aspiration", "agencyengine_impulse_aspiration", true, 120.0f, 480.0f },
        { "relationship", "Relationship", "agencyengine_impulse_relationship", true, 360.0f, 1440.0f },
        { "activity", "Activity", "agencyengine_impulse_activity", true, 240.0f, 2880.0f, true, 3 },
    };
    inline constexpr int kBuiltinLensCount = static_cast<int>(std::size(kBuiltinLenses));
    static_assert(kBuiltinLensCount <= kMaxLenses, "the shipped roster has to fit in the table");

    // The shipped roster as the settings table holds it: builtins first, in
    // roster order, then empty slots for hand-authored lenses.
    inline constexpr std::array<Lens, kMaxLenses> DefaultLenses()
    {
        std::array<Lens, kMaxLenses> table{};
        for (int i = 0; i < kBuiltinLensCount; ++i) {
            table[static_cast<std::size_t>(i)] = kBuiltinLenses[i];
        }
        return table;
    }

    // The shipped row this one is an override of, or nullptr for a lens the user
    // wrote themselves (or one whose id this build no longer knows).
    inline constexpr const Lens* BuiltinLensFor(std::string_view id)
    {
        if (id.empty()) {
            return nullptr;
        }
        for (const auto& builtin : kBuiltinLenses) {
            if (id == builtin.id) {
                return &builtin;
            }
        }
        return nullptr;
    }

    struct Settings
    {
        bool  enabled = true;
        // No global interval any more: cadence is per lens, on Lens::
        // intervalGameMinutes and cooldownGameMinutes. The Director still passes
        // on its own schedule, but a pass only checks clocks — one where nothing
        // is due makes no LLM calls at all. See docs/adr/0003.
        //
        // How many recent SkyrimNet events to feed the prompt.
        int   maxEvents = 40;
        // Announce a fresh carry with a **cue**: a vague direct narration — she
        // seems to have something on her mind — that grants her a speaking turn
        // and names no subject, because her bio already carries the material.
        //
        // One pending cue per companion, coalescing across however many impulses
        // she picks up, which is why the sentence stays vague: a cue that named
        // a subject would be false the moment a second carry joined it.
        //
        // Off is pure drift. Nothing is narrated at all, and the topics she is
        // carrying surface only as they colour what she says when somebody else
        // starts the conversation. That is the built-in fallback rather than a
        // degraded mode — the impulse is in her bio either way — so switching
        // this off costs the timing, not the agenda.
        bool  cues = true;
        // After an impulse is carried, also ask SkyrimNet for a private thought
        // from the speaker about what she has just decided she wants to raise.
        // Costs a second LLM call per carried impulse, and buys the loop its only
        // memory: the thought lands in her event history, which this mod's own
        // prompt reads back on the next ask.
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
        // follower per ask.
        int   perFollowerEvents = 10;
        // Percent chance, 0-100, that an ask is a *forced* impulse: the prompt
        // drops the silence option entirely and someone has to speak up. The
        // rest of the time the model is free to return silence, and normally
        // will, because everything else in the prompt pushes it that way.
        //
        // Injected into every impulse prompt as `forced_impulse_chance` and
        // rolled against `random` there rather than here — the roll has to be
        // suppressed mid-exchange, which the template already knows from
        // `tail_live`, and keeping both halves in one place is what stops them
        // disagreeing. Applies to every lens.
        //
        // 0 never forces (quietest, purely the model's judgement); 100 forces
        // every turn, and it shows — a forced impulse on a thin day is the
        // weakest thing this prompt writes.
        int   forcedImpulseChance = 20;

        // ---- the carried impulse, held in her bio --------------------------
        //
        // Carrying is what delivery *is* now: the stage direction is held in the
        // DLL and rendered into her own character bio by a decorator, verbatim
        // and privately, and the cue above only announces that there is
        // something there.
        //
        // Off falls back to the interim behaviour this replaced, where SkyrimNet
        // is asked to generate a private thought from the impulse as a hint —
        // an LLM call, a paraphrase rather than the text, and no cue, since
        // there would be nothing for her to speak from. Kept as an A/B against
        // the carried version rather than as a supported way to run the mod.
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
        // fair test of "enough has changed". At most one per lens is
        // provisional at a time, because a character holds one pending impulse
        // per lens and a lens only evicts within its own ring.
        int   ledgerSlots = 6;
        // Refuse to carry an impulse whose topic the ledger already holds.
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
        // ---- conversation-aware cues ---------------------------------------
        //
        // Only the cue waits. The impulse itself is carried the moment it comes
        // back, because the store write has to land before any narration does —
        // otherwise the speaking turn arrives before the agenda it is for.
        //
        // An LLM round trip takes 4-8 seconds, so checking for a conversation
        // *before* dispatching protects nothing: the answer arrives long after
        // the check, and the party may have started talking in between. The
        // check that matters is when the cue goes out, which is instantaneous.
        //
        // So the impulse is asked for on schedule, carried the moment it comes
        // back, and only the cue waits for the party to be quiet. Asking up
        // front rather than waiting for a gap and asking then is deliberate — a
        // gap wide enough to start a request is not necessarily still open 4-8
        // seconds later, and chasing it would loop.
        bool  deferOnConversation = true;
        // How long the party must have been silent before a cue goes out,
        // measured against four signals: nobody recording voice input, nothing
        // in the speech/TTS queue, this long since the last NPC audio ended,
        // and this long since anyone took a dialogue turn.
        //
        // The dialogue-turn clock is what makes the number mean anything. On
        // the audio signals alone a conversational pause — you composing a
        // line, then the LLM generating the reply — reads as total silence, and
        // 17s was routinely reached in the middle of an exchange.
        float quietSeconds = 25.0f;
        // How long since the last conversational turn before the exchange counts
        // as *over* rather than merely paused.
        //
        // A second, longer threshold on the same clock, and the two ask different
        // questions. quietSeconds asks whether anybody is mid-utterance; this asks
        // whether there is still a conversation going on around the gap. Twenty-five
        // seconds of nobody speaking is an ordinary beat in a group chat — you
        // composing a longer line, or reading two followers' replies — and a cue
        // fired into it does not read as a companion finding an opening. It reads
        // as one changing the subject.
        //
        // The same argument as the vanilla-dialogue hold above it, applied to
        // SkyrimNet's own conversations: an exchange with a gap in it is still an
        // exchange. Unlike that hold this one *is* inside deferOnConversation,
        // because a conversation with the party is exactly what that switch is
        // about — cutting across a quest NPC is not a preference, and this is.
        //
        // Set below quietSeconds it does nothing, which is the escape hatch for
        // anybody who wants the old behaviour without switching deferral off.
        float conversationSettleSeconds = 100.0f;
        // How long a pending cue waits for that silence before it is dropped.
        //
        // Dropping it costs the announcement and nothing else: the impulse
        // itself landed in her bio before the cue was ever set, so it goes on
        // colouring what she says. There is no degrade path to configure any
        // more, because there is nothing left to degrade *to*.
        float maxDeferSeconds = 60.0f;
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

        // Per-pass trace lines (snapshot contents, gate evaluation, full
        // context and response payloads). Off by default: the Director passes
        // once a second, so this is the difference between a log you can read
        // and a log you have to grep.
        bool  debugLog = false;

        // Every impulse is asked through a lens; there is no general prompt
        // behind them. Switching them all off stops the loop rather than falling
        // back to anything, and the hold says so.
        //
        // The shipped roster, plus whatever the config overrode on top of it.
        // See kBuiltinLenses for why the roster lives in the build.
        std::array<Lens, kMaxLenses> lenses = DefaultLenses();

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
        // The enabled lenses and their cadence, for the line above.
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
