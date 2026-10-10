# Writes build_commit.h with the current commit, run on every build by the
# mv_build_commit target (hook/CMakeLists.txt explains why build time).
#
#   cmake -DGIT=<git> -DSOURCE_DIR=<repo> -DTEMPLATE=<build_commit.h.in>
#         -DOUT=<build_commit.h> -P stamp_build_commit.cmake

execute_process(
  COMMAND ${GIT} rev-parse --short HEAD
  WORKING_DIRECTORY ${SOURCE_DIR}
  OUTPUT_VARIABLE MV_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR MV_GIT_COMMIT STREQUAL "")
  message(FATAL_ERROR "git rev-parse --short HEAD failed - is this a git checkout with at least one commit?")
endif()

# Untracked files count: a new source file not yet added is still part of
# what got built. Ignored files (temp/, testgames/) do not.
execute_process(
  COMMAND ${GIT} status --porcelain
  WORKING_DIRECTORY ${SOURCE_DIR}
  OUTPUT_VARIABLE status
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "git status failed - cannot tell whether this build matches ${MV_GIT_COMMIT}")
endif()
if(NOT status STREQUAL "")
  string(APPEND MV_GIT_COMMIT "-dirty")
endif()

# configure_file leaves OUT untouched when the content is unchanged, so this
# costs no recompile on a clean rebuild.
configure_file(${TEMPLATE} ${OUT} @ONLY)
