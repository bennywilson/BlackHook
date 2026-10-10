#include <windows.h>

#include <string>

#include "build_info.h"
#include "capture.h"
#include "d3d12_hook.h"
#include "depth_identify.h"
#include "logging.h"
#include "overlay.h"
#include "surface_registry.h"
#include "velocity_identify.h"

// MvIdentified*Resource() functions are used by the test harness (testhost/)
// after placing several look alike buffers. Returns nullptr while search is 
// running or when it refused to 
extern "C" __declspec(dllexport) void* MvIdentifiedVelocityResource() {
    return mv::IdentifiedVelocityResource();
}

extern "C" __declspec(dllexport) void* MvIdentifiedDepthResource() {
    return mv::IdentifiedDepthResource();
}

// Drains the writer thread's queue so a host can count dump files right
// afterward and get a complete, deterministic answer.
extern "C" __declspec(dllexport) void MvFlushCapture() {
    mv::Log("mv_hook: MvFlushCapture - draining writer queue");
    mv::ShutdownCapture(/*processExiting=*/false);
}

// Test-only trigger for docs/REFACTOR_PLAN.md Phase 8's depth-reallocation
// behaviour test: forces depth's own standalone reopen cascade, the same one
// ValidateCandidate failing on the selected depth resource would trigger in
// the real hook. A real D3D12 resource's descriptor is immutable once
// created, so testhost cannot organically reproduce "the selected resource's
// descriptor changed underneath us" - this reaches the identical code path
// (SurfaceRegistry::ReopenCascade("depth", ...)) directly instead, so the
// test verifies the actual recovery machinery rather than a synthetic stand-
// in for it.
extern "C" __declspec(dllexport) void MvForceReopenDepth() {
    mv::GetSurfaceRegistry().ReopenCascade(
        "depth", mv::ReopenReason::DescriptorChanged, "MvForceReopenDepth (test-only trigger)");
}

// Test-only trigger for docs/REFACTOR_PLAN.md Phase 8's mid-burst-reopen
// behaviour test: reopens velocity directly, without touching the swapchain.
// A real swapchain resize also changes the back buffer's own descriptor,
// which the PRE-EXISTING (not Phase 8) DumpLayoutAccepts(color, ...) check
// in capture.cpp usually notices - and ends the burst via BurstEndReason::
// LayoutChanged - before velocity's own resize poll (every 30 frames) even
// runs, since that check runs on every Present with no poll interval. That
// makes a real resize an unreliable way to exercise SurfaceRegistry's OWN
// mid-burst-reopen path (NotifySurfaceReopened -> BurstEndReason::
// SurfaceReopened, finding 1.14) in isolation, since the older mechanism
// usually wins the race. This reaches the identical reopen cascade a real
// descriptor mismatch would trigger, with nothing about the back buffer
// disturbed, so the two mechanisms can be told apart.
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
            // Only ever reached at process exit: nothing in this project calls
            // FreeLibrary on this module (MvFlushCapture drains the writer
            // queue without unloading - see testhost/main.cpp).
            mv::ShutdownCapture(/*processExiting=*/true);
            mv::StopOverlayHotkeyThread();

            // WriteFile on the calling thread, not a wait on another one.
            // Without it the last (up to) 500 lines of every batched log are
            // lost, which is reliably the part you wanted when something went
            // wrong at the end of a session.
            mv::FlushLogs();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
