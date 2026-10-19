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

// The profile identification runs under, or "" if none loaded, so the harness
// can tell a failed load from a search that found nothing.
extern "C" __declspec(dllexport) const char* MvActiveProfile() {
    const mv::SurfaceProfile* profile = mv::ActiveSurfaceProfile();
    return profile != nullptr ? profile->name.c_str() : "";
}

// Where mv_hook.log is, with a trailing separator, so the harness can check
// what the hook logged.
extern "C" __declspec(dllexport) const char* MvLogDirectory() {
    static const std::string dir = mv::OutputDir("logs");
    return dir.c_str();
}

// Drains the writer queue, so the harness can count dump files afterwards.
extern "C" __declspec(dllexport) void MvFlushCapture() {
    mv::Log("mv_hook: MvFlushCapture - draining writer queue");
    mv::ShutdownCapture(/*processExiting=*/false);
}

// Test only: depth's reopen cascade, as a changed descriptor would trigger it.
// A real resource's descriptor cannot change, so the testhost cannot cause one.
extern "C" __declspec(dllexport) void MvForceReopenDepth() {
    mv::GetSurfaceRegistry().ReopenCascade(
        "depth", mv::ReopenReason::DescriptorChanged, "MvForceReopenDepth (test-only trigger)");
}

// Test only: velocity's reopen cascade, without touching the swapchain. A real
// resize also changes the back buffer, and the colour layout check usually
// ends the burst first (layout_changed); this tests surface_reopened.
extern "C" __declspec(dllexport) void MvForceReopenVelocity() {
    mv::GetSurfaceRegistry().ReopenCascade(
        "velocity", mv::ReopenReason::DescriptorChanged, "MvForceReopenVelocity (test-only trigger)");
}

namespace {

// Setup runs on its own thread: `DllMain` holds the loader lock, and loading
// libraries or waiting on threads under it can deadlock.
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
            // No DLL_THREAD_ATTACH/DETACH calls for the game's threads.
            DisableThreadLibraryCalls(hModule);
            const HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (thread != nullptr) {
                CloseHandle(thread);
            }
            break;
        }
        case DLL_PROCESS_DETACH: {
            // Reached only at process exit: nothing unloads this module.
            mv::ShutdownCapture(/*processExiting=*/true);
            mv::StopOverlayHotkeyThread();

            // Otherwise the last (up to) 500 lines of each batched log are lost.
            mv::FlushLogs();
            break;
        }
        default:
            break;
    }

    return TRUE;
}
