add_executable(mv_injector src/injector.cpp)

target_compile_definitions(mv_injector PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(mv_injector PRIVATE /W4)

# Runtime library set globally in the top-level CMakeLists.txt.
