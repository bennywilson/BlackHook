#include "logging.h"

#include "build_info.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mv
{
namespace
{

// Resolved once: the configured root if it exists or can be created, else
// %TEMP%.
const std::string& OutputRoot()
{
    static const std::string root = []
    {
        std::error_code ec;
        std::filesystem::create_directories(kOutputRoot, ec);
        if (std::filesystem::is_directory(kOutputRoot, ec))
        {
            return std::string(kOutputRoot) + "/";
        }

        char tempPath[MAX_PATH]{};
        GetTempPathA(MAX_PATH, tempPath);
        return std::string(tempPath);
    }();
    return root;
}

std::mutex& LogMutex()
{
    static std::mutex m;
    return m;
}

std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const double seconds = std::chrono::duration<double>(now).count();
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    return buf;
}

constexpr int kFlushEvery = 500;

// `std::ofstream` rather than shared-write access: the launcher refuses to
// inject twice, so only one process opens a given log.
struct NamedFile
{
    std::ofstream stream;
    int unflushed = 0;
};

std::unordered_map<std::string, NamedFile>& NamedLogFiles()
{
    static std::unordered_map<std::string, NamedFile> files;
    return files;
}

// Caller must hold `LogMutex()`.
NamedFile* FileFor(const std::string& name)
{
    auto& files = NamedLogFiles();
    auto it = files.find(name);
    if (it == files.end())
    {
        NamedFile nf;
        nf.stream.open(OutputDir("logs") + "mv_" + name + ".log", std::ios::app);
        it = files.emplace(name, std::move(nf)).first;
    }

    return it->second.stream.is_open() ? &it->second : nullptr;
}

} // namespace

std::string OutputDir(const std::string& subdir)
{
    std::string dir = OutputRoot() + subdir + "/";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

void Log(const std::string& message)
{
    std::lock_guard<std::mutex> lock(LogMutex());
    NamedFile* f = FileFor("hook");
    if (!f)
    {
        return;
    }

    f->stream << "[" << Timestamp() << "] " << message << "\n";
    f->stream.flush();
}

void LogTo(const std::string& name, const std::string& message)
{
    std::lock_guard<std::mutex> lock(LogMutex());
    NamedFile* f = FileFor(name);
    if (!f)
    {
        return;
    }

    f->stream << "[" << Timestamp() << "] " << message << "\n";
    if (++f->unflushed >= kFlushEvery)
    {
        f->stream.flush();
        f->unflushed = 0;
    }
}

bool DiagEnabled()
{
    static const bool enabled = GetEnvironmentVariableA("MV_DIAG", nullptr, 0) != 0;
    return enabled;
}

void FlushLogs()
{
    std::lock_guard<std::mutex> lock(LogMutex());
    for (auto& [name, file] : NamedLogFiles())
    {
        file.stream.flush();
    }
}

} // namespace mv
