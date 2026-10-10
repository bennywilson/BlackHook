#pragma once
#include <string>

namespace mv
{

// Minimal file logger.
void Log(const std::string& message);

// Used for higher-volume investigative dumps (e.g. render-target binding info) kept
// out of the main log so it stays readable.
void LogTo(const std::string& name, const std::string& message);

void FlushLogs();

// <repo>/temp/<subdir>/, created if missing, with a trailing separator. Falls
// back to %TEMP%\<subdir>\ when the repo's temp/ can't be created - e.g. a DLL
// copied to a machine that doesn't have this checkout.
std::string OutputDir(const std::string& subdir);

} // namespace mv
