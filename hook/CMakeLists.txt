# The manifest (docs/REFACTOR_PLAN.md sec 4.1) records which build produced a
# dump, so a stale build cannot be quietly mistaken for the one under test.
# Real by construction: git failing is a build error, never a placeholder
# string that reads like a real commit.
find_package(Git QUIET)
if(NOT Git_FOUND)
  message(FATAL_ERROR "git is required to build mv_hook (build_commit.h embeds the commit hash)")
endif()

# The commit is stamped at BUILD time, every build, not at configure time: a
# configure-time stamp went stale after every commit on a branch (it only
# re-ran when .git/HEAD itself changed, which a commit does not do), so dumps
# named a commit the DLL was not built from. "-dirty" marks a build with
# uncommitted or untracked changes. The script rewrites build_commit.h only
# when the value changes, so an unchanged tree recompiles nothing.
set(MV_BUILD_COMMIT_H ${CMAKE_CURRENT_BINARY_DIR}/generated/build_commit.h)
add_custom_target(mv_build_commit
  COMMAND ${CMAKE_COMMAND}
    -DGIT=${GIT_EXECUTABLE}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/src/build_commit.h.in
    -DOUT=${MV_BUILD_COMMIT_H}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/stamp_build_commit.cmake
  BYPRODUCTS ${MV_BUILD_COMMIT_H}
  COMMENT "Stamping mv_hook with the current git commit"
  VERBATIM)

string(TIMESTAMP MV_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
# Where the hook writes logs and default dumps: the repo's gitignored temp/,
# so a run never scatters output across %TEMP%. Baked in because the hook
# runs inside the game's process and has no other way to find the repo.
set(MV_OUTPUT_ROOT "${CMAKE_SOURCE_DIR}/temp")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h @ONLY)

# Surface profiles, embedded byte-for-byte so the DLL's commit hash determines
# them (MV_PROFILE_FILE can still add/replace at runtime - surface_profile.h).
# A byte array rather than a string literal: no MSVC literal-length limit, no
# escaping. Editing the JSON re-runs configure via CMAKE_CONFIGURE_DEPENDS.
set(MV_PROFILES_JSON ${CMAKE_SOURCE_DIR}/profiles/ue5.json)
file(READ ${MV_PROFILES_JSON} MV_PROFILES_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MV_PROFILES_BYTES "${MV_PROFILES_HEX}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_profiles.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_profiles.h @ONLY)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MV_PROFILES_JSON})

add_library(mv_hook SHARED
  src/dllmain.cpp
  src/logging.cpp
  src/d3d12_hook.cpp
  src/resource_tracking.cpp
  src/surface_identify.cpp
  src/surface_registry.cpp
  src/surface_profile.cpp
  src/capture.cpp
  src/overlay.cpp
)

target_include_directories(mv_hook PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_dependencies(mv_hook mv_build_commit)
target_link_libraries(mv_hook PRIVATE minhook nlohmann_json::nlohmann_json d3d12 dxgi d3dcompiler)

target_compile_definitions(mv_hook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mv_hook PRIVATE /W4)

# Static CRT (this DLL gets loaded into the game's process and must not
# depend on a specific VC++ redistributable being present there) is set
# globally in the top-level CMakeLists.txt, before MinHook is fetched, so
# every target - including MinHook's - uses the same runtime library.
