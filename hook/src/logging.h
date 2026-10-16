#pragma once
#include <string>

namespace mv
{

// Writes to temp/logs/mv_hook.log, flushed per line.
void Log(const std::string& message);

// Writes to temp/logs/mv_<name>.log, flushed every 500 lines: high-volume
// output kept out of the main log.
void LogTo(const std::string& name, const std::string& message);

void FlushLogs();

// `MV_DIAG` set: barrier-batch tallies (`<surface>-diag:` lines) and
// mv_candidates.log. Off by default, since it takes locks on the barrier hot
// path. Read once.
bool DiagEnabled();

// <repo>/temp/<subdir>/, created if missing, with a trailing separator. Falls
// back to %TEMP%\<subdir>\ when temp/ cannot be created (a DLL copied to a
// machine without this checkout).
std::string OutputDir(const std::string& subdir);

} // namespace mv
