#include "resource_tracking.h"

#include "logging.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mv {
namespace {

// shared_mutex (multiple concurrent readers, exclusive writer) since
// OMSetRenderTargets - the hot-path reader - fires far more often than
// resources/views get created.
std::shared_mutex g_resourceMutex;
std::unordered_map<ID3D12Resource*, ResourceInfo> g_resourceMap;

std::shared_mutex g_rtvMutex;
std::unordered_map<SIZE_T, ID3D12Resource*> g_rtvMap;

std::mutex g_seenMutex;
std::unordered_set<ID3D12Resource*> g_seenResources;

// Bounds worst-case exposure: GetDesc() on a resource encountered in a barrier
// is a considered bet (steady-state resource, not one being created) but not a
// proven-safe pattern, so cap how many distinct resources we are willing to try
// it on. 8000 is generous for identification, which completes in the first ~90
// frames; it is NOT generous for a session, because UE5's transient allocator
// hands out fresh ID3D12Resource objects for the whole run.
constexpr int64_t kIdentifyBudget = 8000;
std::atomic<int64_t> g_identifyBudget{kIdentifyBudget};
std::atomic<bool> g_budgetExhaustionReported{false};

} // namespace

void RecordResource(ID3D12Resource* resource, const ResourceInfo& info) {
    std::unique_lock lock(g_resourceMutex);
    g_resourceMap[resource] = info;
}

std::optional<ResourceInfo> GetResourceInfo(ID3D12Resource* resource) {
    std::shared_lock lock(g_resourceMutex);
    const auto it = g_resourceMap.find(resource);
    if (it == g_resourceMap.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RecordRenderTargetView(SIZE_T descriptorHandle, ID3D12Resource* resource) {
    std::unique_lock lock(g_rtvMutex);
    g_rtvMap[descriptorHandle] = resource;
}

ID3D12Resource* GetResourceForRenderTargetView(SIZE_T descriptorHandle) {
    std::shared_lock lock(g_rtvMutex);
    const auto it = g_rtvMap.find(descriptorHandle);
    return it != g_rtvMap.end() ? it->second : nullptr;
}

bool TryMarkResourceSeen(ID3D12Resource* resource) {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    return g_seenResources.insert(resource).second;
}

bool TryTakeIdentifyBudget() {
    if (g_identifyBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        return true;
    }
    if (!g_budgetExhaustionReported.exchange(true)) {
        Log("resource-tracking: identification budget of " + std::to_string(kIdentifyBudget) +
            " distinct resources is SPENT. No resource created from here on is examined, so a "
            "velocity or depth texture the engine allocates now can never be found - and until this "
            "line existed that happened in total silence, looking exactly like a game that had "
            "stopped creating resources. Identification itself is long finished; what is lost is the "
            "ability to notice a REPLACEMENT.");
    }
    return false;
}

void ForgetAllResourcesSeen() {
    // Refill first: reopening with an empty budget clears the seen set and then
    // rejects every resource one layer down, which searches forever and finds
    // nothing while reporting that it reopened.
    g_identifyBudget.store(kIdentifyBudget, std::memory_order_relaxed);
    g_budgetExhaustionReported.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_seenMutex);
    g_seenResources.clear();
}

} // namespace mv
