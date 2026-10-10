cmake_minimum_required(VERSION 3.21)
project(ue5_motion_vectors LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "x64 only - the target game is x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere, set globally before fetching MinHook so its target
# inherits the same runtime library - mv_hook links statically against
# MinHook and needs a consistent CRT throughout the DLL (mismatched
# static/dynamic CRT linkage between a DLL and what it statically links is
# a real source of heap-corruption-class bugs, not just a linker warning).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
)
FetchContent_MakeAvailable(minhook)

# Parses profiles/ue5.json inside the hook (Phase 9). Header-only; used once,
# at startup, never on a hot path.
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

add_subdirectory(hook)
add_subdirectory(injector)
add_subdirectory(testhost)
