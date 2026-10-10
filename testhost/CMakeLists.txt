add_executable(mv_testhost src/main.cpp src/wpo_encode_test.cpp)

target_link_libraries(mv_testhost PRIVATE d3d12 dxgi d3dcompiler)

target_compile_definitions(mv_testhost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  # Test-only profiles (--phase9-negative), read from the source tree.
  MV_TESTHOST_PROFILE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/profiles")
target_compile_options(mv_testhost PRIVATE /W4)
