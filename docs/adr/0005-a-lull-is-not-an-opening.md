# A lull is not an opening: the settle, and the floor

A companion changes the subject in the middle of a conversation the player is having with the party.
Two independent mechanisms produce it, and they are fixed separately.

**The cue fires into a lull.** ADR 0004 has the cue wait for a conversational lull. What it actually
waits for is `IsQuiet()` — nobody recording voice input, an empty speech queue, and `quietSeconds`
since the last NPC audio and the last dialogue turn. Every one of those signals is *instantaneous*,
and 25 seconds of nobody speaking is an ordinary beat in a group chat: the player composing a longer
line, or reading two followers' replies. The gate answers "is anybody speaking right now", which is
not the question. The question is whether there is still a conversation going on around the gap.

This is the same failure as the vanilla-dialogue hold, which was written when the player talking to a
quest NPC read as *maximally* quiet to every signal. The generalisation was there to be taken and
wasn't: an exchange with a gap in it is still an exchange, whoever is having it.

So a second, longer threshold on the same clock — **`conversationSettleSeconds`**, 100 by default —
sits alongside `quietSeconds` rather than replacing it. The two ask different questions and both
answers are needed: quiet says nobody is mid-utterance, the settle says the exchange has ended. A cue
waits for both. Unlike the vanilla-dialogue hold this one is *inside* `deferOnConversation`, because a
conversation with the party is exactly what that switch is a preference about — cutting across a quest
NPC is the mod talking over the game, and is not.

The settle only works with its other half. The defer clock stops while the settle runs, exactly as it
already stopped for a suspend and for vanilla dialogue: the hold runs 100 seconds past the last word
against a `maxDeferSeconds` of 60, so leaving that clock running would not fix the interruption, it
would delete the cue instead — every one set during a chat expiring before the gate it waits on could
open. Time the party spent talking is not time the cue spent failing to find a gap.

**The bio steers on every turn.** The second mechanism involves no cue at all, produces no log line,
and is the one a player is more likely to see. The carried block in `7200_pending_impulse.prompt`
closed by telling her the subject was "where you steer when the talk gives you room" — rendered into
every dialogue call she made, for as long as she carried it. Mid-conversation a model reads a pause
for breath as room.

The block was conflating two states that are not the same: **carrying** a subject, and **looking for a
way in**. Only the second belongs in her prompt, and only when the mod actually granted the turn. So
the closing branches on a third decorator, `agencyengine_has_the_floor`: with a turn granted, take it;
without one, the subject colours how she says whatever she is already saying and she is not hunting
for an opening.

What the DLL records is the **grant**, not the cue. `PumpPendingCues` erases the cue at the instant it
dispatches the narration, and SkyrimNet renders her bio some way after that — so a decorator asking
"is a cue outstanding" would answer false on the one call the whole mechanism exists for.

## Considered Options

- **Raise `quietSeconds`.** Rejected, and it is the tempting one because it needs no code. It is the
  right *number* attached to the wrong predicate: `IsQuiet` also answers `tail_live` and the injected
  quiet gap, so raising it tells the model the party is mid-exchange a minute after it stopped, and it
  delays every cue into genuine silence by the same amount. Two questions, two thresholds. It remains
  the diagnostic anyone can run from the settings page without a new build.
- **A conversation state machine — open on the first turn, close after N seconds of silence.**
  Rejected as the same predicate with more state to get wrong. Open-since adds nothing the elapsed
  clock does not already carry, and a machine has a wrong state to be stuck in after a load; the
  threshold does not.
- **Hedge the bio's wording unconditionally** ("most of the time it will not come up"). Rejected, and
  previously tried: it is a second layer of suppression on top of the impulse prompt's own, which has
  already decided the subject is worth carrying by the time the bio renders. It suppresses the cued
  turn — the one moment the mod wants her forward — as hard as it suppresses the interruption.
- **Have the DLL return the framing text itself**, one decorator, two wordings. Rejected: the prose
  belongs in the prompt file, which is where it can be edited and diffed without a rebuild. The
  decorator returns the state and the template owns the words.
- **Read the cue directly from the decorator** rather than recording a grant. Does not work — see
  above; the cue is gone before the prompt renders.

## Consequences

- **`conversationSettleSeconds`**, float, default 100, real seconds. A standard field add
  (`Settings.h`, `Load`, `Save`, `Summary`, UI) and under ADR 0002 a shipped default that reaches
  every install that hasn't moved it. Set below `quietSeconds` it is a no-op, which is the escape
  hatch for anybody who wants the old timing without switching deferral off.
- **`agencyengine_has_the_floor`** joins the two existing bio decorators. Inja resolves decorator
  names at *parse* time, so the prompt files and the DLL move together: an install that updates
  `statics/` without the DLL loses its whole character bio, not one block of it. Registered on
  `kDataLoaded` beside the others for that reason.
- The grant window is a constant (30 real seconds), not a setting. It is generous on purpose — she
  stays forward for a line or two past the turn she was given, which is what a person raising
  something does — and the failure modes either side of it are gentle, unlike the settle's.
- No new persistent state. The grant is real-time and in-memory, and is cleared on load with
  everything else `PendingImpulses::Reset` drops: a turn granted in the save you left is not one she
  holds in this one.
- Cues are now held for as long as the party keeps talking, with no upper bound in real time — that
  is what stopping the defer clock means. The bound that remains is the carried impulse's own TTL,
  and the existing resolve gate still drops a cue whose subjects the exchange settled. Drift is the
  fallback throughout, as in ADR 0004: the impulse is in her bio either way.
- The implementation must touch two documents this ADR does not edit: the CONTEXT.md glossary —
  **settle** and **the floor** enter the language, and **cue** rewords from "it waits for a lull",
  which is now precisely the thing it does not do — and the README's description of when a cue goes
  out.

## Open validations

- **Is 100 seconds the right settle?** It was chosen as "long enough that no ordinary beat in an
  exchange reaches it", not measured. Too long and a companion who had something to say never finds
  a gap on a talkative evening; too short and this ADR has not fixed anything. The Conversation panel
  reports the hold, so the evidence is on screen rather than in the log.
- **Does the carried closing now under-steer?** It was the same sentence doing both jobs, and
  splitting it means the non-granted branch can be written as weakly as it likes — including too
  weakly for drift to still work, which is ADR 0004's whole fallback. The symptom would be a carried
  subject that never colours anything until a cue fires.
