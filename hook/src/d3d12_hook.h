#pragma once

namespace mv {

// Creates a throwaway D3D12 device, swapchain and command list to read their
// vtables, hooks the functions behind them with MinHook (which hooks the game's
// instances too), and wires the surface registry. Call once, from a background
// thread.
bool InstallD3D12Hooks();

} // namespace mv
