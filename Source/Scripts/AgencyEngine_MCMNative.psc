Scriptname AgencyEngine_MCMNative Hidden
{Native access to AgencyEngine's live settings. The DLL remains the single
source of truth shared by SkyUI MCM and SKSE Menu Framework.}

Function RefreshStatusView() Global Native
String Function GetStatusSummary() Global Native
Int Function GetStatusRowCount() Global Native
String Function GetStatusRowLabel(Int index) Global Native
String Function GetStatusRowValue(Int index) Global Native
Function RequestImpulseNow() Global Native
Function RestartImpulseTimer() Global Native

Function RefreshPendingView() Global Native
String Function GetPendingSummary() Global Native
Int Function GetPendingCount() Global Native
String Function GetPendingLabel(Int index) Global Native
String Function GetPendingValue(Int index) Global Native
String Function GetPendingDetails(Int index) Global Native
Bool Function CheckPending(Int index) Global Native
Bool Function StopPending(Int index) Global Native
Bool Function ForgetPending(Int index) Global Native
Int Function CheckAllPending() Global Native
Int Function ForgetAllPending() Global Native
Int Function GetPendingQueued() Global Native

Function RefreshLedgerView() Global Native
String Function GetLedgerSummary() Global Native
Int Function GetLedgerCount() Global Native
String Function GetLedgerLabel(Int index) Global Native
String Function GetLedgerValue(Int index) Global Native
String Function GetLedgerDetails(Int index) Global Native

Function RefreshHistoryView() Global Native
String Function GetHistorySummary() Global Native
Int Function GetHistoryCount() Global Native
String Function GetHistoryLabel(Int index) Global Native
String Function GetHistoryValue(Int index) Global Native
String Function GetHistoryDetails(Int index) Global Native
String Function GetLastContext() Global Native

Bool Function GetBool(String key) Global Native
Bool Function SetBool(String key, Bool value) Global Native
Int Function GetInt(String key) Global Native
Bool Function SetInt(String key, Int value) Global Native
Float Function GetFloat(String key) Global Native
Bool Function SetFloat(String key, Float value) Global Native
String Function GetString(String key) Global Native
Bool Function SetString(String key, String value) Global Native

Int Function GetLensCount() Global Native
Bool Function IsLensBuiltin(Int index) Global Native
String Function GetLensName(Int index) Global Native
Bool Function SetLensName(Int index, String value) Global Native
String Function GetLensPrompt(Int index) Global Native
Bool Function SetLensPrompt(Int index, String value) Global Native
Bool Function GetLensEnabled(Int index) Global Native
Bool Function SetLensEnabled(Int index, Bool value) Global Native
Float Function GetLensInterval(Int index) Global Native
Bool Function SetLensInterval(Int index, Float value) Global Native
Float Function GetLensCooldown(Int index) Global Native
Bool Function SetLensCooldown(Int index, Float value) Global Native
Bool Function GetLensProposal(Int index) Global Native
Bool Function SetLensProposal(Int index, Bool value) Global Native
Int Function GetLensSlots(Int index) Global Native
Bool Function SetLensSlots(Int index, Int value) Global Native
Bool Function RequestLens(Int index) Global Native

Function RestoreLensDefaults() Global Native
Function ResetSettings() Global Native
Bool Function ReloadSettings() Global Native
Bool Function SaveSettings() Global Native
