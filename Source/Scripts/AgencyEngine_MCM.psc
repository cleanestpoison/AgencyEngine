Scriptname AgencyEngine_MCM extends SKI_ConfigBase
{SkyUI configuration frontend for AgencyEngine. Every value is read from and
written to the DLL so this menu and SKSE Menu Framework share one live state.}

Int Function GetVersion()
	Return 5
EndFunction

String[] _boolKeys
Int[] _boolOids
Bool[] _boolDefaults

String[] _intKeys
Int[] _intOids
Int[] _intMinimums
Int[] _intMaximums
Int[] _intDefaults
Int[] _intSteps
String[] _intFormats

String[] _floatKeys
Int[] _floatOids
Float[] _floatMinimums
Float[] _floatMaximums
Float[] _floatDefaults
Float[] _floatSteps
String[] _floatFormats

String[] _stringKeys
Int[] _stringOids
String[] _stringDefaults

Int[] _lensEnabledOids
Int[] _lensIntervalOids
Int[] _lensCooldownOids
Int[] _lensSlotsOids
Int[] _lensAskOids

Int[] _pendingViewOids
Int[] _pendingCheckOids
Int[] _pendingForgetOids
Int[] _ledgerViewOids
Int[] _historyViewOids

Int _statusGenerateOID = -1
Int _statusRestartOID = -1
Int _pendingCheckAllOID = -1
Int _pendingForgetAllOID = -1
Int _lastContextOID = -1
Int _previousOID = -1
Int _nextOID = -1
Int _carriedOffset = 0
Int _ledgerOffset = 0
String _activeParityPage = ""

Int _resetOID = -1
Int _reloadOID = -1
Int _restoreLensOID = -1

Event OnConfigInit()
	ModName = "Agency Engine"
	InitializePages()
	InitializeSettingTables()
EndEvent

Event OnVersionUpdate(Int version)
	If version >= 2
		InitializePages()
	EndIf
	If version >= 1
		InitializeSettingTables()
	EndIf
EndEvent

Function InitializePages()
	Pages = New String[10]
	Pages[0] = "Status"
	Pages[1] = "Impulses"
	Pages[2] = "Speaking Up"
	Pages[3] = "Combat"
	Pages[4] = "Lenses"
	Pages[5] = "Context"
	Pages[6] = "Diagnostics"
	Pages[7] = "Carried"
	Pages[8] = "Ledger"
	Pages[9] = "History"
EndFunction

Function InitializeSettingTables()
	_boolKeys = New String[12]
	_boolOids = New Int[12]
	_boolDefaults = New Bool[12]
	_boolKeys[0] = "enabled"
	_boolDefaults[0] = True
	_boolKeys[1] = "cues"
	_boolDefaults[1] = True
	_boolKeys[2] = "generateThought"
	_boolDefaults[2] = True
	_boolKeys[3] = "requireFollower"
	_boolDefaults[3] = True
	_boolKeys[4] = "skipInCombat"
	_boolDefaults[4] = True
	_boolKeys[5] = "pendingBioInjection"
	_boolDefaults[5] = True
	_boolKeys[6] = "ledgerEnabled"
	_boolDefaults[6] = True
	_boolKeys[7] = "ledgerVeto"
	_boolDefaults[7] = True
	_boolKeys[8] = "combatContinuousMode"
	_boolDefaults[8] = False
	_boolKeys[9] = "deferOnConversation"
	_boolDefaults[9] = True
	_boolKeys[10] = "injectQuietGap"
	_boolDefaults[10] = True
	_boolKeys[11] = "debugLog"
	_boolDefaults[11] = False

	_intKeys = New String[4]
	_intOids = New Int[4]
	_intMinimums = New Int[4]
	_intMaximums = New Int[4]
	_intDefaults = New Int[4]
	_intSteps = New Int[4]
	_intFormats = New String[4]
	ConfigureInt(0, "maxEvents", 5, 200, 40, 5, "{0}")
	ConfigureInt(1, "perFollowerEvents", 0, 120, 10, 1, "{0}")
	ConfigureInt(2, "forcedImpulseChance", 0, 100, 20, 1, "{0}%")
	ConfigureInt(3, "ledgerSlots", 1, 20, 6, 1, "{0}")

	_floatKeys = New String[7]
	_floatOids = New Int[7]
	_floatMinimums = New Float[7]
	_floatMaximums = New Float[7]
	_floatDefaults = New Float[7]
	_floatSteps = New Float[7]
	_floatFormats = New String[7]
	ConfigureFloat(0, "pendingTtlGameMinutes", 30.0, 4320.0, 720.0, 30.0, "{0} min")
	ConfigureFloat(1, "pendingResolveGameMinutes", 0.0, 720.0, 180.0, 15.0, "{0} min")
	ConfigureFloat(2, "continuousExitGraceSeconds", 0.0, 60.0, 10.0, 1.0, "{0} s")
	ConfigureFloat(3, "quietSeconds", 0.0, 60.0, 25.0, 1.0, "{0} s")
	ConfigureFloat(4, "conversationSettleSeconds", 0.0, 300.0, 100.0, 5.0, "{0} s")
	ConfigureFloat(5, "maxDeferSeconds", 5.0, 300.0, 60.0, 5.0, "{0} s")
	ConfigureFloat(6, "quietPollSeconds", 0.25, 5.0, 1.0, 0.25, "{2} s")

	_stringKeys = New String[2]
	_stringOids = New Int[2]
	_stringDefaults = New String[2]
	_stringKeys[0] = "eventTypeFilter"
	_stringDefaults[0] = ""
	_stringKeys[1] = "followerEventTypeFilter"
	_stringDefaults[1] = "npc_thoughts"

	; The DLL reports the current shipped roster; sixteen entries leave room for
	; future built-in lenses without a saved-script migration.
	_lensEnabledOids = New Int[16]
	_lensIntervalOids = New Int[16]
	_lensCooldownOids = New Int[16]
	_lensSlotsOids = New Int[16]
	_lensAskOids = New Int[16]

	; Papyrus arrays are capped at 128. Page carried entries so its three
	; actions per row remain below that limit; ledger rows page separately.
	_pendingViewOids = New Int[15]
	_pendingCheckOids = New Int[15]
	_pendingForgetOids = New Int[15]
	_ledgerViewOids = New Int[40]
	_historyViewOids = New Int[25]

	ClearOptionIds()
EndFunction

Function ConfigureInt(Int index, String settingKey, Int minimum, Int maximum, Int defaultValue, Int step, String format)
	_intKeys[index] = settingKey
	_intMinimums[index] = minimum
	_intMaximums[index] = maximum
	_intDefaults[index] = defaultValue
	_intSteps[index] = step
	_intFormats[index] = format
EndFunction

Function ConfigureFloat(Int index, String settingKey, Float minimum, Float maximum, Float defaultValue, Float step, String format)
	_floatKeys[index] = settingKey
	_floatMinimums[index] = minimum
	_floatMaximums[index] = maximum
	_floatDefaults[index] = defaultValue
	_floatSteps[index] = step
	_floatFormats[index] = format
EndFunction

Function ClearOptionIds()
	Int index = 0
	While index < _boolOids.Length
		_boolOids[index] = -1
		index += 1
	EndWhile

	index = 0
	While index < _intOids.Length
		_intOids[index] = -1
		index += 1
	EndWhile

	index = 0
	While index < _floatOids.Length
		_floatOids[index] = -1
		index += 1
	EndWhile

	index = 0
	While index < _stringOids.Length
		_stringOids[index] = -1
		index += 1
	EndWhile

	index = 0
	While index < _lensEnabledOids.Length
		_lensEnabledOids[index] = -1
		_lensIntervalOids[index] = -1
		_lensCooldownOids[index] = -1
		_lensSlotsOids[index] = -1
		_lensAskOids[index] = -1
		index += 1
	EndWhile

	index = 0
	While index < _pendingViewOids.Length
		_pendingViewOids[index] = -1
		_pendingCheckOids[index] = -1
		_pendingForgetOids[index] = -1
		index += 1
	EndWhile

	index = 0
	While index < _ledgerViewOids.Length
		_ledgerViewOids[index] = -1
		index += 1
	EndWhile

	index = 0
	While index < _historyViewOids.Length
		_historyViewOids[index] = -1
		index += 1
	EndWhile

	_statusGenerateOID = -1
	_statusRestartOID = -1
	_pendingCheckAllOID = -1
	_pendingForgetAllOID = -1
	_lastContextOID = -1
	_previousOID = -1
	_nextOID = -1
	_resetOID = -1
	_reloadOID = -1
	_restoreLensOID = -1
EndFunction

Event OnPageReset(String page)
	; Both arrays live in the save. Refresh Pages as well as option lookup tables:
	; SkyUI may retain an older page list even after this script is upgraded.
	InitializePages()
	InitializeSettingTables()

	ClearOptionIds()
	SetCursorFillMode(TOP_TO_BOTTOM)

	Bool configurationPage = False
	If page == "" || page == "Status"
		_activeParityPage = "Status"
		BuildStatusPage()
	ElseIf page == "Impulses"
		configurationPage = True
		BuildImpulsesPage()
	ElseIf page == "Speaking Up"
		configurationPage = True
		BuildSpeakingPage()
	ElseIf page == "Combat"
		configurationPage = True
		BuildCombatPage()
	ElseIf page == "Lenses"
		configurationPage = True
		BuildLensesPage()
	ElseIf page == "Context"
		configurationPage = True
		BuildContextPage()
	ElseIf page == "Diagnostics"
		configurationPage = True
		BuildDiagnosticsPage()
	ElseIf page == "Carried"
		_activeParityPage = "Carried"
		BuildCarriedPage()
	ElseIf page == "Ledger"
		_activeParityPage = "Ledger"
		BuildLedgerPage()
	ElseIf page == "History"
		_activeParityPage = "History"
		BuildHistoryPage()
	ElseIf page == "Custom Lenses"
		; One draw can arrive from SkyUI's stale list before the refreshed Pages
		; property is read. Give that obsolete page useful content meanwhile.
		_activeParityPage = "Status"
		BuildStatusPage()
	EndIf

	If configurationPage
		AddPersistenceControls()
	EndIf
EndEvent

Int Function AddBoolSetting(Int index, String label, Int flags = 0)
	Bool value = AgencyEngine_MCMNative.GetBool(_boolKeys[index])
	_boolOids[index] = AddToggleOption(label, value, flags)
	Return _boolOids[index]
EndFunction

Int Function AddIntSetting(Int index, String label, Int flags = 0)
	Int value = AgencyEngine_MCMNative.GetInt(_intKeys[index])
	_intOids[index] = AddSliderOption(label, value, _intFormats[index], flags)
	Return _intOids[index]
EndFunction

Int Function AddFloatSetting(Int index, String label, Int flags = 0)
	Float value = AgencyEngine_MCMNative.GetFloat(_floatKeys[index])
	_floatOids[index] = AddSliderOption(label, value, _floatFormats[index], flags)
	Return _floatOids[index]
EndFunction

Int Function AddStringSetting(Int index, String label, Int flags = 0)
	String value = AgencyEngine_MCMNative.GetString(_stringKeys[index])
	_stringOids[index] = AddInputOption(label, value, flags)
	Return _stringOids[index]
EndFunction

Function BuildImpulsesPage()
	AddHeaderOption("Impulse generation")
	; Papyrus interns string literals case-insensitively. Keep this label from
	; aliasing the native key "enabled" in the compiled string table.
	AddBoolSetting(0, "Master switch")
	AddBoolSetting(3, "Only when a follower is present")
	AddIntSetting(2, "Force someone to speak")
	AddBoolSetting(2, "Also generate a private thought")

	AddHeaderOption("What she goes on carrying")
	AddBoolSetting(5, "Hold the impulse in her character bio")
	Int carryFlags = OPTION_FLAG_NONE
	If !AgencyEngine_MCMNative.GetBool("pendingBioInjection")
		carryFlags = OPTION_FLAG_DISABLED
	EndIf
	AddFloatSetting(0, "Forget it after", carryFlags)
	AddFloatSetting(1, "Check whether it is still live every", carryFlags)

	AddHeaderOption("Already raised")
	AddBoolSetting(6, "Remember what each companion has raised")
	Int ledgerFlags = OPTION_FLAG_NONE
	If !AgencyEngine_MCMNative.GetBool("ledgerEnabled")
		ledgerFlags = OPTION_FLAG_DISABLED
	EndIf
	AddIntSetting(3, "Subjects remembered per companion", ledgerFlags)
	AddBoolSetting(7, "Refuse an impulse that repeats one", ledgerFlags)
EndFunction

Function BuildSpeakingPage()
	AddHeaderOption("Speaking up")
	AddBoolSetting(1, "Announce a fresh impulse with a cue")

	Int cueFlags = OPTION_FLAG_NONE
	If !AgencyEngine_MCMNative.GetBool("cues")
		cueFlags = OPTION_FLAG_DISABLED
	EndIf
	AddBoolSetting(9, "Wait for a gap before cueing her", cueFlags)

	Int waitFlags = cueFlags
	If !AgencyEngine_MCMNative.GetBool("deferOnConversation")
		waitFlags = OPTION_FLAG_DISABLED
	EndIf
	AddFloatSetting(3, "Silence required", waitFlags)
	AddFloatSetting(4, "Conversation is over after", waitFlags)
	AddFloatSetting(5, "Give up after", waitFlags)

	AddHeaderOption("What the model is told")
	AddBoolSetting(10, "Tell it how long the party has been quiet", cueFlags)
EndFunction

Function BuildCombatPage()
	AddHeaderOption("Combat")
	AddBoolSetting(4, "Skip impulses while in combat")
	AddBoolSetting(8, "Hold SkyrimNet continuous mode during combat")

	Int continuousFlags = OPTION_FLAG_NONE
	If !AgencyEngine_MCMNative.GetBool("combatContinuousMode")
		continuousFlags = OPTION_FLAG_DISABLED
	EndIf
	AddFloatSetting(2, "Grace before switching off", continuousFlags)
EndFunction

Function BuildLensesPage()
	AddHeaderOption("Lenses")

	Int count = AgencyEngine_MCMNative.GetLensCount()
	Int index = 0
	While index < count && index < _lensEnabledOids.Length
		If AgencyEngine_MCMNative.IsLensBuiltin(index)
			AddLens(index)
		EndIf
		index += 1
	EndWhile

	_restoreLensOID = AddTextOption("Restore shipped lens defaults", "RESTORE")
EndFunction

Function AddLens(Int index)
	String name = AgencyEngine_MCMNative.GetLensName(index)
	String prompt = AgencyEngine_MCMNative.GetLensPrompt(index)
	Bool enabled = AgencyEngine_MCMNative.GetLensEnabled(index)
	Float interval = AgencyEngine_MCMNative.GetLensInterval(index)
	Float cooldown = AgencyEngine_MCMNative.GetLensCooldown(index)
	Int slots = AgencyEngine_MCMNative.GetLensSlots(index)

	AddHeaderOption(name)

	Int enabledFlags = OPTION_FLAG_NONE
	If prompt == ""
		enabledFlags = OPTION_FLAG_DISABLED
	EndIf
	_lensEnabledOids[index] = AddToggleOption("Lens enabled", enabled, enabledFlags)
	_lensIntervalOids[index] = AddSliderOption("Interval", interval, "{0} min")
	_lensCooldownOids[index] = AddSliderOption("Cooldown", cooldown, "{0} min")
	_lensSlotsOids[index] = AddSliderOption("Remembered subjects (0 = shared)", slots, "{0}")

	Int askFlags = enabledFlags
	If !enabled
		askFlags = OPTION_FLAG_DISABLED
	EndIf
	_lensAskOids[index] = AddTextOption("Ask this lens now", "ASK", askFlags)
EndFunction

Function BuildContextPage()
	AddHeaderOption("Recent context sent to the model")
	AddIntSetting(0, "Player events")
	AddIntSetting(1, "Thoughts per follower")
	AddStringSetting(0, "Player event type filter")
	AddStringSetting(1, "Follower event type filter")
EndFunction

Function BuildDiagnosticsPage()
	AddHeaderOption("Conversation bridge")
	Int pollFlags = OPTION_FLAG_NONE
	If !AgencyEngine_MCMNative.GetBool("deferOnConversation") && !AgencyEngine_MCMNative.GetBool("injectQuietGap")
		pollFlags = OPTION_FLAG_DISABLED
	EndIf
	AddFloatSetting(6, "Conversation poll interval", pollFlags)

	AddHeaderOption("Logging")
	AddBoolSetting(11, "Verbose pass logging")

	AddHeaderOption("Configuration file")
	_reloadOID = AddTextOption("Reload from disk", "RELOAD")
EndFunction

Function BuildStatusPage()
	AgencyEngine_MCMNative.RefreshStatusView()

	AddHeaderOption("Current state")
	String summary = AgencyEngine_MCMNative.GetStatusSummary()
	AddTextOption(summary, "", OPTION_FLAG_DISABLED)
	_statusGenerateOID = AddTextOption("Generate an impulse now", "RUN")
	_statusRestartOID = AddTextOption("Restart every lens timer", "RESTART")

	AddHeaderOption("Live details")
	Int count = AgencyEngine_MCMNative.GetStatusRowCount()
	Int index = 0
	While index < count
		String label = AgencyEngine_MCMNative.GetStatusRowLabel(index)
		String value = AgencyEngine_MCMNative.GetStatusRowValue(index)
		AddTextOption(label, value, OPTION_FLAG_DISABLED)
		index += 1
	EndWhile
EndFunction

Function BuildCarriedPage()
	AgencyEngine_MCMNative.RefreshPendingView()
	Int count = AgencyEngine_MCMNative.GetPendingCount()
	If _carriedOffset >= count
		_carriedOffset = 0
	EndIf

	AddHeaderOption("Carried, unsaid")
	String summary = AgencyEngine_MCMNative.GetPendingSummary()
	AddTextOption(summary, "", OPTION_FLAG_DISABLED)

	If count > 0
		_pendingCheckAllOID = AddTextOption("Check all now", "CHECK")
		_pendingForgetAllOID = AddTextOption("Forget all", "FORGET")
	EndIf
	Int queued = AgencyEngine_MCMNative.GetPendingQueued()
	If queued > 0
		AddTextOption("Resolution checks queued", queued, OPTION_FLAG_DISABLED)
	EndIf

	Int slot = 0
	While slot < _pendingViewOids.Length && (_carriedOffset + slot) < count
		Int row = _carriedOffset + slot
		String label = AgencyEngine_MCMNative.GetPendingLabel(row)
		String value = AgencyEngine_MCMNative.GetPendingValue(row)
		AddHeaderOption(label)
		_pendingViewOids[slot] = AddTextOption(value, "VIEW")
		_pendingCheckOids[slot] = AddTextOption("Run its still-live check", "CHECK")
		_pendingForgetOids[slot] = AddTextOption("Forget this impulse", "FORGET")
		slot += 1
	EndWhile

	AddPageControls(_carriedOffset, count, _pendingViewOids.Length)
EndFunction

Function BuildLedgerPage()
	AgencyEngine_MCMNative.RefreshLedgerView()
	Int count = AgencyEngine_MCMNative.GetLedgerCount()
	If _ledgerOffset >= count
		_ledgerOffset = 0
	EndIf

	AddHeaderOption("Already raised")
	String summary = AgencyEngine_MCMNative.GetLedgerSummary()
	AddTextOption(summary, "", OPTION_FLAG_DISABLED)

	Int slot = 0
	While slot < _ledgerViewOids.Length && (_ledgerOffset + slot) < count
		Int row = _ledgerOffset + slot
		String label = AgencyEngine_MCMNative.GetLedgerLabel(row)
		String value = AgencyEngine_MCMNative.GetLedgerValue(row)
		_ledgerViewOids[slot] = AddTextOption(label, value)
		slot += 1
	EndWhile

	AddPageControls(_ledgerOffset, count, _ledgerViewOids.Length)
EndFunction

Function BuildHistoryPage()
	AgencyEngine_MCMNative.RefreshHistoryView()
	Int count = AgencyEngine_MCMNative.GetHistoryCount()

	AddHeaderOption("Recent asks")
	String summary = AgencyEngine_MCMNative.GetHistorySummary()
	AddTextOption(summary, "", OPTION_FLAG_DISABLED)

	Int index = 0
	While index < count && index < _historyViewOids.Length
		String label = AgencyEngine_MCMNative.GetHistoryLabel(index)
		String value = AgencyEngine_MCMNative.GetHistoryValue(index)
		_historyViewOids[index] = AddTextOption(label, value)
		index += 1
	EndWhile

	String context = AgencyEngine_MCMNative.GetLastContext()
	If context != ""
		AddHeaderOption("Diagnostics")
		_lastContextOID = AddTextOption("Last context sent to the model", "VIEW")
	EndIf
EndFunction

Function AddPageControls(Int offset, Int count, Int pageSize)
	If offset > 0
		_previousOID = AddTextOption("Previous page", "PREVIOUS")
	EndIf
	If offset + pageSize < count
		_nextOID = AddTextOption("Next page", "NEXT")
	EndIf
EndFunction

Function AddPersistenceControls()
	AddHeaderOption("Persistence")
	_resetOID = AddTextOption("Reset to shipped defaults", "RESET")
	AddTextOption("Changes are saved automatically", "", OPTION_FLAG_DISABLED)
EndFunction

Function PersistSettings()
	If !AgencyEngine_MCMNative.SaveSettings()
		ShowMessage("AgencyEngine could not write its settings file. Check AgencyEngine.log.", False)
	EndIf
EndFunction

Event OnOptionSelect(Int option)
	Int index = -1

	If option == _statusGenerateOID
		AgencyEngine_MCMNative.RequestImpulseNow()
		ShowMessage("AgencyEngine will ask the nearest-due lens on its next pass.", False)
		Return
	ElseIf option == _statusRestartOID
		AgencyEngine_MCMNative.RestartImpulseTimer()
		ShowMessage("Every lens timer has been restarted from the current game time.", False)
		ForcePageReset()
		Return
	ElseIf option == _pendingCheckAllOID
		Int queued = AgencyEngine_MCMNative.CheckAllPending()
		ShowMessage(queued + " still-live check(s) queued.", False)
		ForcePageReset()
		Return
	ElseIf option == _pendingForgetAllOID
		If ShowMessage("Forget every carried impulse? Each companion will stop carrying these subjects immediately.", True, "$Yes", "$No")
			Int forgotten = AgencyEngine_MCMNative.ForgetAllPending()
			ShowMessage(forgotten + " impulse(s) forgotten.", False)
			ForcePageReset()
		EndIf
		Return
	ElseIf option == _lastContextOID
		String context = AgencyEngine_MCMNative.GetLastContext()
		ShowMessage(context, False)
		Return
	ElseIf option == _previousOID
		If _activeParityPage == "Carried"
			_carriedOffset -= _pendingViewOids.Length
			If _carriedOffset < 0
				_carriedOffset = 0
			EndIf
		ElseIf _activeParityPage == "Ledger"
			_ledgerOffset -= _ledgerViewOids.Length
			If _ledgerOffset < 0
				_ledgerOffset = 0
			EndIf
		EndIf
		ForcePageReset()
		Return
	ElseIf option == _nextOID
		If _activeParityPage == "Carried"
			_carriedOffset += _pendingViewOids.Length
		ElseIf _activeParityPage == "Ledger"
			_ledgerOffset += _ledgerViewOids.Length
		EndIf
		ForcePageReset()
		Return
	EndIf

	index = FindOption(_pendingViewOids, option)
	If index >= 0
		String details = AgencyEngine_MCMNative.GetPendingDetails(_carriedOffset + index)
		ShowMessage(details, False)
		Return
	EndIf

	index = FindOption(_pendingCheckOids, option)
	If index >= 0
		If AgencyEngine_MCMNative.CheckPending(_carriedOffset + index)
			ShowMessage("The still-live check is queued.", False)
			ForcePageReset()
		EndIf
		Return
	EndIf

	index = FindOption(_pendingForgetOids, option)
	If index >= 0
		String pendingLabel = AgencyEngine_MCMNative.GetPendingLabel(_carriedOffset + index)
		If ShowMessage("Forget " + pendingLabel + "? This immediately removes it from the companion's bio.", True, "$Yes", "$No")
			AgencyEngine_MCMNative.ForgetPending(_carriedOffset + index)
			ForcePageReset()
		EndIf
		Return
	EndIf

	index = FindOption(_ledgerViewOids, option)
	If index >= 0
		String ledgerDetails = AgencyEngine_MCMNative.GetLedgerDetails(_ledgerOffset + index)
		ShowMessage(ledgerDetails, False)
		Return
	EndIf

	index = FindOption(_historyViewOids, option)
	If index >= 0
		String historyDetails = AgencyEngine_MCMNative.GetHistoryDetails(index)
		ShowMessage(historyDetails, False)
		Return
	EndIf

	index = FindOption(_boolOids, option)
	If index >= 0
		Bool value = !AgencyEngine_MCMNative.GetBool(_boolKeys[index])
		If AgencyEngine_MCMNative.SetBool(_boolKeys[index], value)
			SetToggleOptionValue(option, value)
			PersistSettings()
			If index == 1 || index == 5 || index == 6 || index == 8 || index == 9 || index == 10
				ForcePageReset()
			EndIf
		EndIf
		Return
	EndIf

	index = FindOption(_lensEnabledOids, option)
	If index >= 0
		Bool lensEnabled = !AgencyEngine_MCMNative.GetLensEnabled(index)
		If AgencyEngine_MCMNative.SetLensEnabled(index, lensEnabled)
			SetToggleOptionValue(option, lensEnabled)
			PersistSettings()
			ForcePageReset()
		EndIf
		Return
	EndIf

	index = FindOption(_lensAskOids, option)
	If index >= 0
		If AgencyEngine_MCMNative.RequestLens(index)
			ShowMessage("The lens will ask on AgencyEngine's next pass.", False)
		Else
			ShowMessage("That lens is disabled or has no prompt file.", False)
		EndIf
		Return
	EndIf

	If option == _resetOID
		If ShowMessage("Reset every AgencyEngine setting to the values shipped by this version? The reset is saved immediately.", True, "$Yes", "$No")
			AgencyEngine_MCMNative.ResetSettings()
			PersistSettings()
			ForcePageReset()
		EndIf
	ElseIf option == _reloadOID
		If AgencyEngine_MCMNative.ReloadSettings()
			ShowMessage("Settings reloaded from disk.", False)
			ForcePageReset()
		Else
			ShowMessage("No settings file could be loaded. Current live values were left unchanged; check AgencyEngine.log.", False)
		EndIf
	ElseIf option == _restoreLensOID
		If ShowMessage("Restore every shipped lens switch, interval, cooldown, and slot count?", True, "$Yes", "$No")
			AgencyEngine_MCMNative.RestoreLensDefaults()
			PersistSettings()
			ForcePageReset()
		EndIf
	EndIf
EndEvent

Event OnOptionSliderOpen(Int option)
	Int index = FindOption(_intOids, option)
	If index >= 0
		SetSliderDialogStartValue(AgencyEngine_MCMNative.GetInt(_intKeys[index]))
		SetSliderDialogDefaultValue(_intDefaults[index])
		SetSliderDialogRange(_intMinimums[index], _intMaximums[index])
		SetSliderDialogInterval(_intSteps[index])
		Return
	EndIf

	index = FindOption(_floatOids, option)
	If index >= 0
		SetSliderDialogStartValue(AgencyEngine_MCMNative.GetFloat(_floatKeys[index]))
		SetSliderDialogDefaultValue(_floatDefaults[index])
		SetSliderDialogRange(_floatMinimums[index], _floatMaximums[index])
		SetSliderDialogInterval(_floatSteps[index])
		Return
	EndIf

	index = FindOption(_lensIntervalOids, option)
	If index >= 0
		SetSliderDialogStartValue(AgencyEngine_MCMNative.GetLensInterval(index))
		SetSliderDialogDefaultValue(120.0)
		SetSliderDialogRange(15.0, 2880.0)
		SetSliderDialogInterval(15.0)
		Return
	EndIf

	index = FindOption(_lensCooldownOids, option)
	If index >= 0
		SetSliderDialogStartValue(AgencyEngine_MCMNative.GetLensCooldown(index))
		SetSliderDialogDefaultValue(480.0)
		SetSliderDialogRange(0.0, 5760.0)
		SetSliderDialogInterval(30.0)
		Return
	EndIf

	index = FindOption(_lensSlotsOids, option)
	If index >= 0
		SetSliderDialogStartValue(AgencyEngine_MCMNative.GetLensSlots(index))
		SetSliderDialogDefaultValue(0.0)
		SetSliderDialogRange(0.0, 20.0)
		SetSliderDialogInterval(1.0)
	EndIf
EndEvent

Event OnOptionSliderAccept(Int option, Float value)
	Int index = FindOption(_intOids, option)
	If index >= 0
		If AgencyEngine_MCMNative.SetInt(_intKeys[index], value As Int)
			SetSliderOptionValue(option, AgencyEngine_MCMNative.GetInt(_intKeys[index]), _intFormats[index])
			PersistSettings()
		EndIf
		Return
	EndIf

	index = FindOption(_floatOids, option)
	If index >= 0
		If AgencyEngine_MCMNative.SetFloat(_floatKeys[index], value)
			SetSliderOptionValue(option, AgencyEngine_MCMNative.GetFloat(_floatKeys[index]), _floatFormats[index])
			PersistSettings()
		EndIf
		Return
	EndIf

	index = FindOption(_lensIntervalOids, option)
	If index >= 0
		If AgencyEngine_MCMNative.SetLensInterval(index, value)
			SetSliderOptionValue(option, AgencyEngine_MCMNative.GetLensInterval(index), "{0} min")
			PersistSettings()
		EndIf
		Return
	EndIf

	index = FindOption(_lensCooldownOids, option)
	If index >= 0
		If AgencyEngine_MCMNative.SetLensCooldown(index, value)
			SetSliderOptionValue(option, AgencyEngine_MCMNative.GetLensCooldown(index), "{0} min")
			PersistSettings()
		EndIf
		Return
	EndIf

	index = FindOption(_lensSlotsOids, option)
	If index >= 0
		If AgencyEngine_MCMNative.SetLensSlots(index, value As Int)
			SetSliderOptionValue(option, AgencyEngine_MCMNative.GetLensSlots(index), "{0}")
			PersistSettings()
		EndIf
	EndIf
EndEvent

Event OnOptionInputOpen(Int option)
	Int index = FindOption(_stringOids, option)
	If index >= 0
		SetInputDialogStartText(AgencyEngine_MCMNative.GetString(_stringKeys[index]))
	EndIf
EndEvent

Event OnOptionInputAccept(Int option, String value)
	Int index = FindOption(_stringOids, option)
	If index >= 0
		If !AgencyEngine_MCMNative.SetString(_stringKeys[index], value)
			ShowMessage("The value was too long and has been shortened.", False)
		EndIf
		SetInputOptionValue(option, AgencyEngine_MCMNative.GetString(_stringKeys[index]))
		PersistSettings()
	EndIf
EndEvent

Event OnOptionDefault(Int option)
	Int index = FindOption(_boolOids, option)
	If index >= 0
		AgencyEngine_MCMNative.SetBool(_boolKeys[index], _boolDefaults[index])
		SetToggleOptionValue(option, _boolDefaults[index])
		PersistSettings()
		Return
	EndIf

	index = FindOption(_intOids, option)
	If index >= 0
		AgencyEngine_MCMNative.SetInt(_intKeys[index], _intDefaults[index])
		SetSliderOptionValue(option, _intDefaults[index], _intFormats[index])
		PersistSettings()
		Return
	EndIf

	index = FindOption(_floatOids, option)
	If index >= 0
		AgencyEngine_MCMNative.SetFloat(_floatKeys[index], _floatDefaults[index])
		SetSliderOptionValue(option, _floatDefaults[index], _floatFormats[index])
		PersistSettings()
		Return
	EndIf

	index = FindOption(_stringOids, option)
	If index >= 0
		AgencyEngine_MCMNative.SetString(_stringKeys[index], _stringDefaults[index])
		SetInputOptionValue(option, _stringDefaults[index])
		PersistSettings()
	EndIf
EndEvent

Event OnOptionHighlight(Int option)
	Int index = -1

	If option == _statusGenerateOID
		SetInfoText("Ask the nearest-due enabled lens on AgencyEngine's next pass and spend its normal clock.")
		Return
	ElseIf option == _statusRestartOID
		SetInfoText("Rearm every lens from the current game time.")
		Return
	ElseIf option == _pendingCheckAllOID
		SetInfoText("Queue one still-live LLM check per open impulse. Checks run one at a time.")
		Return
	ElseIf option == _pendingForgetAllOID
		SetInfoText("Immediately remove every carried impulse from every companion's bio.")
		Return
	ElseIf option == _lastContextOID
		SetInfoText("Show the exact context payload most recently sent to the impulse model.")
		Return
	ElseIf option == _previousOID || option == _nextOID
		SetInfoText("Move through entries without dropping any from the view.")
		Return
	EndIf

	index = FindOption(_pendingViewOids, option)
	If index >= 0
		SetInfoText("Open the full impulse text, subject, state, age, expiry, and next resolution check.")
		Return
	EndIf
	index = FindOption(_pendingCheckOids, option)
	If index >= 0
		SetInfoText("Queue one LLM check asking whether this subject is still live.")
		Return
	EndIf
	index = FindOption(_pendingForgetOids, option)
	If index >= 0
		SetInfoText("Immediately remove this impulse from the companion's bio.")
		Return
	EndIf
	index = FindOption(_ledgerViewOids, option)
	If index >= 0
		SetInfoText("Show this remembered subject, its lens ring, capacity, and whether it awaits a verdict.")
		Return
	EndIf
	index = FindOption(_historyViewOids, option)
	If index >= 0
		SetInfoText("Open the complete impulse result, subject, delivery path, and lens.")
		Return
	EndIf

	index = FindOption(_boolOids, option)
	If index >= 0
		SetInfoText(BoolHelp(index))
		Return
	EndIf

	index = FindOption(_intOids, option)
	If index >= 0
		SetInfoText(IntHelp(index))
		Return
	EndIf

	index = FindOption(_floatOids, option)
	If index >= 0
		SetInfoText(FloatHelp(index))
		Return
	EndIf

	index = FindOption(_stringOids, option)
	If index >= 0
		SetInfoText("Comma-separated SkyrimNet event types. Empty means every type; the follower filter falls back to the player filter when empty.")
		Return
	EndIf

	index = FindOption(_lensEnabledOids, option)
	If index >= 0
		SetInfoText("Whether this lens asks at all. Disable a lens whose prompt depends on content you do not have installed.")
		Return
	EndIf

	index = FindOption(_lensIntervalOids, option)
	If index >= 0
		SetInfoText("In-game minutes between asks. Every ask costs an LLM call even when the result is silence.")
		Return
	EndIf

	index = FindOption(_lensCooldownOids, option)
	If index >= 0
		SetInfoText("Extra in-game minutes added after this lens produces an impulse.")
		Return
	EndIf

	index = FindOption(_lensSlotsOids, option)
	If index >= 0
		SetInfoText("Subjects remembered for this lens per companion. Zero uses the shared setting on the Impulses page.")
		Return
	EndIf


	index = FindOption(_lensAskOids, option)
	If index >= 0
		SetInfoText("Ask this lens on AgencyEngine's next pass regardless of its clock. This spends its next natural ask.")
		Return
	EndIf

	If option == _resetOID
		SetInfoText("Reset every setting to this version's shipped default and save the result immediately.")
	ElseIf option == _reloadOID
		SetInfoText("Replace live settings with the values currently stored in AgencyEngine.json.")
	ElseIf option == _restoreLensOID
		SetInfoText("Restore shipped lens controls and save them without changing any other setting.")
	Else
		SetInfoText("")
	EndIf
EndEvent

Int Function FindOption(Int[] options, Int option)
	Int index = 0
	While index < options.Length
		If options[index] == option
			Return index
		EndIf
		index += 1
	EndWhile
	Return -1
EndFunction

String Function BoolHelp(Int index)
	If index == 0
		Return "Master switch. Off, the loop keeps running but no lens is asked."
	ElseIf index == 1
		Return "Send a vague cue when a companion receives a fresh impulse. Off leaves the subject in her bio to surface naturally."
	ElseIf index == 2
		Return "After a delivered impulse, spend a second LLM call to generate a private thought for its speaker."
	ElseIf index == 3
		Return "Do not ask for impulses when the player has no follower present."
	ElseIf index == 4
		Return "Do not ask for impulses during combat. Lens clocks continue running."
	ElseIf index == 5
		Return "Keep each impulse verbatim in its speaker's private character bio until it is resolved or expires."
	ElseIf index == 6
		Return "Remember subjects each companion already raised so prompts and the veto can prevent repetition."
	ElseIf index == 7
		Return "Reject a generated impulse when its subject is already in that companion's ledger. The LLM call has already been spent."
	ElseIf index == 8
		Return "Ask SkyrimNet to hold continuous scene mode during combat and release only the mode AgencyEngine acquired."
	ElseIf index == 9
		Return "Hold a cue until the current conversation has ended and the configured silence has elapsed."
	ElseIf index == 10
		Return "Tell the impulse prompt how long the party has been quiet so it can distinguish a pause from a dead scene."
	ElseIf index == 11
		Return "Log every pass plus full context and response payloads. This produces a large log."
	EndIf
	Return ""
EndFunction

String Function IntHelp(Int index)
	If index == 0
		Return "Maximum recent player events included in each impulse prompt."
	ElseIf index == 1
		Return "Recent events fetched per follower. With the default filter this is effectively a thought count."
	ElseIf index == 2
		Return "Percent of eligible asks where the model is not allowed to answer with silence."
	ElseIf index == 3
		Return "Subjects remembered per companion for lenses that use the shared slot count."
	EndIf
	Return ""
EndFunction

String Function FloatHelp(Int index)
	If index == 0
		Return "How long an unsaid impulse remains in its speaker's bio, measured in in-game minutes."
	ElseIf index == 1
		Return "How often to spend an LLM call checking whether each carried subject has been resolved. Zero disables checks."
	ElseIf index == 2
		Return "Real seconds out of combat before AgencyEngine releases continuous scene mode."
	ElseIf index == 3
		Return "Real seconds of silence required before a held cue can be sent."
	ElseIf index == 4
		Return "Real seconds since the last dialogue turn before the conversation counts as over."
	ElseIf index == 5
		Return "Real seconds a cue may wait once it is otherwise eligible. The impulse remains carried if the cue is dropped."
	ElseIf index == 6
		Return "Real seconds between Papyrus samples of recording, speech queue, and recent audio state."
	EndIf
	Return ""
EndFunction
