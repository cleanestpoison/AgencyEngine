Scriptname AgencyEngine_Bridge Hidden
{Papyrus-side helpers for things SkyrimNet exposes only to Papyrus.

Two jobs: the read-modify-write behind the continuous-mode toggle, and polling
the three "is anyone talking right now" signals, which have no C++ equivalent.

SkyrimNet exposes continuous scene mode as a *toggle* (TriggerToggleContinuousMode)
plus a query (IsContinuousModeEnabled) — there is no setter. Doing the read and the
toggle from C++ would mean two trips across the VM boundary with an unbounded gap
between them, because a native's return value only comes back through an
asynchronous stack callback. The player can press the hotkey in that gap, and then
we flip them the wrong way.

Doing it here keeps the read and the toggle adjacent in one Papyrus stack, so the
window shrinks to the frames this function occupies. C++ dispatches this
fire-and-forget and learns the outcome from the mod event below.}

; Sent to the DLL's SKSE::ModCallbackEvent sink once the work is done. There is no
; other channel: a Papyrus function called from C++ cannot hand a value back
; without the caller waiting on a stack callback, which is the thing this script
; exists to avoid.
;
;   strArg  "acquire" when the DLL asked for continuous mode ON, "release" for OFF
;   numArg  bit 0 (1) = mode was already enabled before the call
;           bit 1 (2) = mode is enabled now
;
; Both bits matter to the caller. before=1 on an acquire means the player already
; had continuous mode on, so the DLL must NOT turn it off again when combat ends.
; after=0 on an acquire means the toggle did nothing, which in practice means the
; GameMaster agent is disabled — SkyrimNet ignores the continuous-mode hotkey
; entirely in that state, and there is no query to ask about it directly.

Function SetContinuousMode(bool desired) global
	bool before = SkyrimNetApi.IsContinuousModeEnabled()

	if before != desired
		SkyrimNetApi.TriggerToggleContinuousMode()
		; The trigger simulates a hotkey press rather than writing the flag, so
		; the new state is not guaranteed to be observable on the same frame.
		; Reading it back too early reports "the toggle did nothing", which the
		; DLL would report to the user as "your GameMaster is off".
		Utility.Wait(0.2)
	endIf

	bool after = SkyrimNetApi.IsContinuousModeEnabled()

	float report = 0.0
	if before
		report += 1.0
	endIf
	if after
		report += 2.0
	endIf

	string op = "release"
	if desired
		op = "acquire"
	endIf

	; SendModEvent is a Form method, and the sink filters on the event name, so
	; the sender is only ever a required argument. The player is the one form
	; guaranteed to exist whenever this can be called.
	Actor player = Game.GetPlayer()
	if player
		player.SendModEvent("AgencyEngine_ContinuousMode", op, report)
	endIf
EndFunction

; Samples the three signals that say whether the party is mid-conversation, and
; reports them in one mod event. All three are Papyrus-only — the C++ API has no
; equivalent, which is why this lives here.
;
;   IsRecordingInput()           the player is holding the microphone RIGHT NOW.
;                                Produces no dialogue events at all, so nothing
;                                on the C++ side can see it.
;   GetSpeechQueueSize()         lines waiting on generation or TTS. Non-zero
;                                covers the several-second window between a line
;                                being decided and it being heard, during which
;                                the event log looks completely silent.
;   GetTimeSinceLastAudioEnded() milliseconds since anyone stopped speaking.
;                                Returns 0 when no audio has played yet — after
;                                a load, say — so 0 must not be read as "someone
;                                just finished talking".
;
; Reported as:
;   strArg  "<recording 0|1>;<speech queue size>"
;   numArg  milliseconds since the last audio ended
;
; Sampling all three in one stack matters: read separately from C++ they would
; be three round trips describing three different moments, and the composite
; answer would never correspond to any instant that actually existed.

Function PollQuiet() global
	bool recording = SkyrimNetApi.IsRecordingInput()
	int queued = SkyrimNetApi.GetSpeechQueueSize()
	int sinceMs = SkyrimNetApi.GetTimeSinceLastAudioEnded()

	string recFlag = "0"
	if recording
		recFlag = "1"
	endIf

	Actor player = Game.GetPlayer()
	if player
		player.SendModEvent("AgencyEngine_Quiet", recFlag + ";" + queued, sinceMs as float)
	endIf
EndFunction
