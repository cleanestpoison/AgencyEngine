cmake_minimum_required(VERSION 3.21)

project(AgencyEngine VERSION 0.2.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Output folder (mod-manager staging)
# ---------------------------------------------------------------------------
# SKYRIM_MODS_FOLDER is set by CMakeUserPresets.json to a staging directory
# inside the repo (_deploy/) — deliberately NOT the live MO2 mods folder, so a
# build never mutates the active modlist. Copy across deliberately once tested.
# Both machine-specific paths are seeded from the environment on the FIRST
# configure and then pinned in the cache. Ninja can re-run cmake by itself
# (--regenerate-during-build) when a glob or CMakeLists changes, and that
# re-run does NOT inherit the preset's environment block — so resolving these
# from ENV every time makes an ordinary incremental build fail with "could not
# locate SkyrimNet". Cache variables survive; environment variables don't.
#
# Note the deliberate absence of an IS_DIRECTORY guard on SKYRIM_MODS_FOLDER:
# the staging root is created by the build, so requiring it to pre-exist would
# silently skip every deploy step on a fresh clone and produce a DLL that never
# reaches a mod folder.
if(NOT DEFINED FA_OUTPUT_FOLDER)
    if(DEFINED ENV{SKYRIM_MODS_FOLDER})
        set(FA_OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}"
            CACHE PATH "Mod folder the build deploys into")
    elseif(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
        set(FA_OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data"
            CACHE PATH "Mod folder the build deploys into")
    endif()
endif()

if(FA_OUTPUT_FOLDER)
    set(OUTPUT_FOLDER "${FA_OUTPUT_FOLDER}")
    file(MAKE_DIRECTORY "${OUTPUT_FOLDER}")
endif()

file(GLOB_RECURSE source_files CONFIGURE_DEPENDS src/*.cpp)

# Off by default, so the build a fresh clone runs is exactly the plugin. There
# is one thing here worth testing offline — the ledger, which is pure state and
# can regress a shipped lens with no visible symptom — and it needs neither the
# game nor SkyrimNet to compile. See tests/CMakeLists.txt.
option(AGENCYENGINE_BUILD_TESTS "Build the offline ledger tests" OFF)

# ---------------------------------------------------------------------------
# Papyrus
# ---------------------------------------------------------------------------
# Source/Scripts/*.psc are compiled by hand (see the Papyrus section of
# README.md for the command) and the resulting
# .pex is committed under statics/Scripts, which the deploy step below ships
# like any other static file. The build does not invoke the Papyrus compiler:
# its path is machine-specific and it needs the base-game script headers, so
# depending on it would make a plain C++ build fail on a fresh clone.
#
# The cost of that choice is a .pex that can silently drift behind its source,
# which fails at runtime as a function that doesn't exist rather than as
# anything a build would catch. So: check at configure time and say so.
file(GLOB PAPYRUS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Source/Scripts/*.psc")
foreach(psc IN LISTS PAPYRUS_SOURCES)
    get_filename_component(psc_name "${psc}" NAME_WE)
    set(pex "${CMAKE_SOURCE_DIR}/statics/Scripts/${psc_name}.pex")
    if(NOT EXISTS "${pex}")
        message(WARNING "Papyrus: ${psc_name}.psc has no compiled ${psc_name}.pex in statics/Scripts. "
                        "See the Papyrus section of README.md for the compile command.")
    elseif("${psc}" IS_NEWER_THAN "${pex}")
        message(WARNING "Papyrus: ${psc_name}.psc is newer than statics/Scripts/${psc_name}.pex — "
                        "the shipped script is stale. See the Papyrus section of README.md for the "
                        "compile command.")
    endif()
endforeach()

# CommonLibSSE's exported target names Microsoft::DirectXTK in its link
# interface, so directxtk must be found FIRST or the CommonLibSSE config fails
# at generate time with "target was not found".
find_package(directxtk CONFIG REQUIRED)
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${source_files} plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)

target_include_directories(${PROJECT_NAME} PRIVATE include)

# Vendored SKSEMenuFramework.h — the framework ships no library, the header
# soft-links SKSEMenuFramework.dll via GetModuleHandle. SYSTEM because it's
# 11k lines of generated ImGui forwarding that trips /W4 (struct-vs-class,
# cross-enum operator|); those warnings are upstream's to fix, not ours, and
# they'd otherwise bury our own.
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE external/SKSEMenuFramework)

target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# SkyrimNet integration
# ---------------------------------------------------------------------------
# SkyrimNet's C++ API is a single header (CppAPI/PublicAPI.h) whose inline
# FindFunctions() LoadLibrary's SkyrimNet.dll at runtime. No import lib, so
# the only build-time integration is putting the header on the include path.
if(NOT DEFINED SKYRIMNET_DIR)
    if(DEFINED ENV{SKYRIMNET_DIR} AND IS_DIRECTORY "$ENV{SKYRIMNET_DIR}/CppAPI")
        set(SKYRIMNET_DIR "$ENV{SKYRIMNET_DIR}" CACHE PATH "SkyrimNet mod folder (contains CppAPI/PublicAPI.h)")
    elseif(
        DEFINED ENV{SKYRIM_MODS_FOLDER}
        AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}/SkyrimNet/CppAPI"
    )
        set(SKYRIMNET_DIR "$ENV{SKYRIM_MODS_FOLDER}/SkyrimNet"
            CACHE PATH "SkyrimNet mod folder (contains CppAPI/PublicAPI.h)")
    endif()
endif()

if(NOT SKYRIMNET_DIR)
    message(
        FATAL_ERROR
        "Could not locate SkyrimNet. Set the SKYRIMNET_DIR environment variable to "
        "the SkyrimNet mod folder (the one containing CppAPI/PublicAPI.h)."
    )
endif()

message(STATUS "SkyrimNet found at: ${SKYRIMNET_DIR}")
target_include_directories(${PROJECT_NAME} PRIVATE "${SKYRIMNET_DIR}/CppAPI")

if(AGENCYENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Deploy
# ---------------------------------------------------------------------------
if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND
            "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
                "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()

    # statics/ mirrors the mod-folder layout verbatim; every file under it is
    # wired as its own custom-command output so editing a prompt (no C++ touched)
    # still redeploys on the next build.
    set(STATICS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/statics")
    if(IS_DIRECTORY "${STATICS_SOURCE_DIR}")
        file(
            GLOB_RECURSE STATICS_FILES
            CONFIGURE_DEPENDS
            RELATIVE "${STATICS_SOURCE_DIR}"
            "${STATICS_SOURCE_DIR}/*"
        )
        set(STATICS_DEPLOY_OUTPUTS "")
        foreach(rel IN LISTS STATICS_FILES)
            set(src "${STATICS_SOURCE_DIR}/${rel}")
            set(dst "${OUTPUT_FOLDER}/${rel}")
            get_filename_component(dst_dir "${dst}" DIRECTORY)
            add_custom_command(
                OUTPUT "${dst}"
                DEPENDS "${src}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${dst_dir}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src}" "${dst}"
                VERBATIM
            )
            list(APPEND STATICS_DEPLOY_OUTPUTS "${dst}")
        endforeach()
        if(STATICS_DEPLOY_OUTPUTS)
            add_custom_target(deploy_statics ALL DEPENDS ${STATICS_DEPLOY_OUTPUTS})
            message(STATUS "Statics deploy: ${STATICS_SOURCE_DIR} -> ${OUTPUT_FOLDER}")
        endif()
    endif()
endif()
