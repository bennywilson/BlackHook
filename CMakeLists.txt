cmake_minimum_required(VERSION 3.21)
project(ue5_motion_vectors LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "x64 only - the target game is x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT everywhere, set before MinHook is fetched so it gets the same
# runtime: mv_hook links MinHook statically, and mixed static/dynamic CRT within
# one DLL can corrupt the heap.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
)
FetchContent_MakeAvailable(minhook)

# Parses profiles/ue5.json in the hook, once at startup. Header-only.
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

add_subdirectory(hook)
add_subdirectory(injector)
add_subdirectory(testhost)
