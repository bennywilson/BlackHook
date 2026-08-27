# The manifest (docs/REFACTOR_PLAN.md sec 4.1) records which build produced a
# dump, so a stale build cannot be quietly mistaken for the one under test.
# Real by construction: git rev-parse fails loudly here rather than letting a
# placeholder string reach a manifest looking like a real commit.
find_package(Git QUIET)
if(NOT Git_FOUND)
  message(FATAL_ERROR "git is required to configure mv_hook (build_info.h embeds the commit hash)")
endif()
execute_process(
  COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE MV_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE MV_GIT_COMMIT_RESULT
)
if(NOT MV_GIT_COMMIT_RESULT EQUAL 0 OR MV_GIT_COMMIT STREQUAL "")
  message(FATAL_ERROR "git rev-parse --short HEAD failed - is this a git checkout with at least one commit?")
endif()
string(TIMESTAMP MV_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h @ONLY)
# Re-run configure whenever HEAD moves (commit, checkout, merge), so the
# embedded hash tracks it without a manual `cmake -B build -S .`. Does not
# catch an in-place `git commit --amend` that leaves HEAD's ref target
# unchanged in the same way - CI/local rebuilds after an amend should
# reconfigure explicitly.
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/../.git/HEAD)

add_library(mv_hook SHARED
  src/dllmain.cpp
  src/logging.cpp
  src/d3d12_hook.cpp
  src/resource_tracking.cpp
  src/surface_identify.cpp
  src/velocity_identify.cpp
  src/depth_identify.cpp
  src/capture.cpp
  src/overlay.cpp
)

target_include_directories(mv_hook PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mv_hook PRIVATE minhook d3d12 dxgi d3dcompiler)

target_compile_definitions(mv_hook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mv_hook PRIVATE /W4)

# Static CRT (this DLL gets loaded into the game's process and must not
# depend on a specific VC++ redistributable being present there) is set
# globally in the top-level CMakeLists.txt, before MinHook is fetched, so
# every target - including MinHook's - uses the same runtime library.
