# The ledger test target.
#
# Off unless AGENCYENGINE_BUILD_TESTS is on, so a plain plugin build — the one a
# fresh clone runs — is unaffected by it. src/PendingImpulse.cpp is compiled a
# second time here rather than being linked out of the plugin: the plugin is a
# DLL built against CommonLibSSE and the game's PCH, and neither is available to
# a console executable.
add_executable(AgencyEngineTests
    LedgerTests.cpp
    "${CMAKE_SOURCE_DIR}/src/PendingImpulse.cpp"
    "${CMAKE_SOURCE_DIR}/src/ResolutionScheduler.cpp"
    "${CMAKE_SOURCE_DIR}/src/SkyrimNetEvent.cpp"
)

target_compile_features(AgencyEngineTests PRIVATE cxx_std_23)

# The config, for the same reason: Settings.cpp is plain JSON and plain state,
# and what it gets wrong is an *upgrade*, which is not reproducible by hand once
# the old file has been overwritten. Separate executable because each case
# chdirs into a scratch directory — Settings::FilePath is relative by design.
add_executable(AgencyEngineSettingsTests
    SettingsTests.cpp
    "${CMAKE_SOURCE_DIR}/src/Settings.cpp"
)
target_compile_features(AgencyEngineSettingsTests PRIVATE cxx_std_23)
target_include_directories(AgencyEngineSettingsTests PRIVATE shim "${CMAKE_SOURCE_DIR}/include")
target_precompile_headers(AgencyEngineSettingsTests PRIVATE TestPCH.h)
target_link_libraries(AgencyEngineSettingsTests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME settings COMMAND AgencyEngineSettingsTests)

# Combat episode timing is pure state: the game contributes only timestamped
# IsInCombat/suspension observations. Test the encounter seam without SKSE so
# brief combat drops, pauses, and long stalls stay deterministic.
add_executable(AgencyEngineCombatEpisodeTests
    CombatEpisodeTests.cpp
    "${CMAKE_SOURCE_DIR}/src/CombatEpisode.cpp"
)
target_compile_features(AgencyEngineCombatEpisodeTests PRIVATE cxx_std_23)
target_include_directories(AgencyEngineCombatEpisodeTests PRIVATE shim "${CMAKE_SOURCE_DIR}/include")
target_precompile_headers(AgencyEngineCombatEpisodeTests PRIVATE TestPCH.h)
add_test(NAME combat-episode COMMAND AgencyEngineCombatEpisodeTests)

# Cue dispatch spacing is pure real-time state. Keep its attempt boundary
# offline so two companions becoming eligible in one Director pass cannot
# regress into a narration burst.
add_executable(AgencyEngineCueSpacingTests
    CueSpacingTests.cpp
)
target_compile_features(AgencyEngineCueSpacingTests PRIVATE cxx_std_23)
target_include_directories(AgencyEngineCueSpacingTests PRIVATE shim "${CMAKE_SOURCE_DIR}/include")
target_precompile_headers(AgencyEngineCueSpacingTests PRIVATE TestPCH.h)
add_test(NAME cue-spacing COMMAND AgencyEngineCueSpacingTests)

# shim/ ahead of include/ so that `#include "Logging.h"` from the ledger finds
# the stub rather than the spdlog/SKSE one.
target_include_directories(AgencyEngineTests PRIVATE shim "${CMAKE_SOURCE_DIR}/include")

# Forced-include, not a real precompiled header's worth of work: PendingImpulse.h
# names std types that the game PCH supplies inside the plugin.
target_precompile_headers(AgencyEngineTests PRIVATE TestPCH.h)

target_link_libraries(AgencyEngineTests PRIVATE nlohmann_json::nlohmann_json)

add_test(NAME ledger COMMAND AgencyEngineTests)
