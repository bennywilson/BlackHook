#include <windows.h>

#include <string>

#include "build_info.h"
#include "capture.h"
#include "d3d12_hook.h"
#include "logging.h"
#include "overlay.h"
#include "surface_profile.h"
#include "surface_registry.h"

// The resource a surface ("velocity", "depth", ...) selected, for the test
// harness: nullptr while searching, ambiguous, or not registered.
extern "C" __declspec(dllexport) void* MvIdentifiedSurface(const char* name) {
    return name != nullptr ? mv::GetSurfaceRegistry().Selected(name) : nullptr;
}

// The profile identification is running under, or "" if none loaded - so the
// harness can tell "searched under the profile I asked for and found nothing"
// apart from "never searched because the profile failed to load".
extern "C" __declspec(dllexport) const char* MvActiveProfile() {
    const mv::SurfaceProfile* profile = mv::ActiveSurfaceProfile();
    return profile != nullptr ? profile->name.c_str() : "";
}

// Where mv_hook.log is, with a trailing separator - so the harness can assert
// on what the hook said, not just on what it selected.
extern "C" __declspec(dllexport) const char* MvLogDirectory() {
    static const std::string dir = mv::OutputDir("logs");
    return dir.c_str();
}

// Drains the writer thread's queue so a host can count dump files right
// afterward and get a complete, deterministic answer.
extern "C" __declspec(dllexport) void MvFlushCapture() {
    mv::Log("mv_hook: MvFlushCapture - draining writer queue");
    mv::ShutdownCapture(/*processExiting=*/false);
}

// Test-only: depth's reopen cascade, as a changed descriptor would trigger it
// (a real resource's descriptor cannot change, so testhost cannot cause one).
extern "C" __declspec(dllexport) void MvForceReopenDepth() {
    mv::GetSurfaceRegistry().ReopenCascade(
        "depth", mv::ReopenReason::DescriptorChanged, "MvForceReopenDepth (test-only trigger)");
}

// Test-only: velocity's reopen cascade, without touching the swapchain. A real
// resize also changes the back buffer, and the colour layout check usually
// ends the burst first (layout_changed); this exercises surface_reopened.
extern "C" __declspec(dllexport) void MvForceReopenVelocity() {
    mv::GetSurfaceRegistry().ReopenCascade(
        "velocity", mv::ReopenReason::DescriptorChanged, "MvForceReopenVelocity (test-only trigger)");
}

namespace {

// Initialization is performed on a separate thread instead of DllMain because 
// the OS holds the global Loader Lock while the latter is running.
//
// The Loader Lock is used to protect the list of modules (DLLs and EXEs) loaded
// into a process' memory. Calling Load/FreeLibrary, waiting on threads, or 
// doing complex memory allocations while holding it can cause a deadlock.
DWORD WINAPI InitThread(LPVOID) {
    mv::Log(std::string("mv_hook: injected, InitThread running (build ") + mv::kBuildGitCommit + ")");

    if (!mv::InstallD3D12Hooks()) {
        mv::Log("mv_hook: D3D12 hook installation failed");
    }

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // Suppress DLL_THREAD_ATTACH/DLL_THREAD_DEATH from the target process
            DisableThreadLibraryCalls(hModule);
            const HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (thread != nullptr) {
                CloseHandle(thread);
            }
            break;
        }
        case DLL_PROCESS_DETACH: {
            // Only ever reached at process exit: nothing unloads this module
            // (MvFlushCapture drains the writer without unloading).
            mv::ShutdownCapture(/*processExiting=*/true);
            mv::StopOverlayHotkeyThread();

            // Without this the last (up to) 500 lines of every batched log -
            // usually the ones you want - are lost.
            mv::FlushLogs();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
