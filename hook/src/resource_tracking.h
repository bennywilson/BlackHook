#pragma once

#include <d3d12.h>

#include <optional>

namespace mv {

// What we know about a resource from its creation-time D3D12_RESOURCE_DESC
// - enough to test the velocity buffer's expected signature (2-channel
// half-float, render resolution) without any debug names, mirroring the
// same "identify by structural signature" approach used against DOOM
// Eternal's Vulkan motion vector buffer.
struct ResourceInfo {
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
};

void RecordResource(ID3D12Resource* resource, const ResourceInfo& info);
std::optional<ResourceInfo> GetResourceInfo(ID3D12Resource* resource);

// D3D12_CPU_DESCRIPTOR_HANDLE.ptr -> the resource an RTV was last created
// against for that descriptor slot. Slots get reused across frames (engines
// commonly rebind different resources into the same heap slot over time),
// so this always reflects whatever was most recently written there -
// callers must resolve it at bind time, not cache it.
void RecordRenderTargetView(SIZE_T descriptorHandle, ID3D12Resource* resource);
ID3D12Resource* GetResourceForRenderTargetView(SIZE_T descriptorHandle);

// Marks a resource pointer as "seen" for identification purposes, atomically
// returning true only the first time a given pointer is passed in. Used to
// make sure a resource is only ever queried (e.g. via GetDesc()) once, no
// matter how many times its pointer turns up in barrier data afterward.
//
// KNOWN LIMITATION, stated because it is a real hole rather than a theoretical
// one: this set holds bare pointers and no references, so if a resource is
// released and a later allocation reuses its address, the new resource is
// treated as already-seen and never identified. For candidates that hole is
// closed (they are ref-held, in SurfaceIdentifier's own candidate set); for
// the ~8000 resources the identify pass merely walks past, it is not. The
// failure mode is missing the velocity buffer entirely rather than acting on
// the wrong one - loud, not silent - and in practice identification
// completes within the first few seconds, well before the transient
// allocator has recycled much. Closing it properly means re-running
// GetDesc() on every barrier, which is exactly the unbounded exposure
// g_identifyBudget exists to prevent.
bool TryMarkResourceSeen(ID3D12Resource* resource);

// Takes one unit of the identification budget, returning false once it is
// spent. The budget bounds how many distinct resources we are willing to call
// GetDesc() on - a considered bet rather than a proven-safe pattern - and it
// used to be a bare atomic in d3d12_hook.cpp that ran out in silence.
//
// Silence was the problem. In the skyrunner_reproject session every
// identification log line - shortlists, near-misses, both searches - stopped
// dead at t+96s and nothing was ever printed again for at least the remaining
// 47 minutes the session ran, which is what an exhausted budget looks like from
// the outside and is indistinguishable from "the game stopped creating
// resources". Whether that is
// what happened there is not established; that it CANNOT BE TOLD APART is, and
// this is the line that tells them apart next time.
bool TryTakeIdentifyBudget();

// Empties the "seen" set so every resource is examined again from scratch, and
// refills the budget above.
//
// Only used when identification is reopened: the set exists to make GetDesc() a
// once-per-resource cost, so without clearing it a second search would walk past
// every resource in the process in silence and find nothing. Costs one more
// GetDesc() per live resource, once. As of docs/REFACTOR_PLAN.md Phase 8, called
// exactly once per SurfaceRegistry::ReopenCascade rather than once per
// identifier that happens to clear its own shortlist (finding 1.7) - see
// surface_registry.cpp.
//
// The budget refill is not decoration. Reopening after the budget had run out
// was a guaranteed no-op - the seen set was cleared and then every resource was
// rejected one layer down for want of budget, so the search would have run
// forever and found nothing, loudly reporting that it had reopened.
void ForgetAllResourcesSeen();

} // namespace mv
