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
)

target_compile_features(AgencyEngineTests PRIVATE cxx_std_23)

# shim/ ahead of include/ so that `#include "Logging.h"` from the ledger finds
# the stub rather than the spdlog/SKSE one.
target_include_directories(AgencyEngineTests PRIVATE shim "${CMAKE_SOURCE_DIR}/include")

# Forced-include, not a real precompiled header's worth of work: PendingImpulse.h
# names std types that the game PCH supplies inside the plugin.
target_precompile_headers(AgencyEngineTests PRIVATE TestPCH.h)

target_link_libraries(AgencyEngineTests PRIVATE nlohmann_json::nlohmann_json)

add_test(NAME ledger COMMAND AgencyEngineTests)
