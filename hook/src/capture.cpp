#include "capture.h"

#include "build_info.h"
#include "logging.h"
#include "surface_profile.h"
#include "surface_registry.h"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace mv
{
namespace
{

// Frames per F8 burst: ~20s of gameplay at the ~120fps measured on Skyrunner
// at 960x540.
constexpr int kCaptureFrames = 2400;

// Slots in flight. GPU trails CPU by a few frames, so copies aren't
// immediately readable; round-robin and map only once the fence passes.
constexpr int kSlots = 4;

// A Recording slot waits for the Present sharing its frame index. If that
// Present never comes (abandoned frame, resolution change, skipped Present),
// reclaim it rather than stall capture forever.
constexpr unsigned long long kSlotStaleAfterFrames = 8;

// Cap on depth copies per frame ("last one wins"). UE5 normally transitions
// depth out of DEPTH_WRITE 1-2x/frame; the cap bounds bandwidth if a title's
// frame graph does something unexpected.
constexpr int kMaxDepthCopiesPerFrame = 4;

// Recording from the first copy (velocity or depth, whichever comes first)
// until the Present that pairs it with the back buffer and submits.
enum class SlotState
{
    Free,
    Recording,
    Submitted
};

// What capture needs per surface (kSurfaceNames order). trigger/arbitration/
// file pattern are reported in manifest.json as-is.
struct CaptureFacts
{
    const char* filePrefix; // vel_00001.bin
    const char* trigger;
    const char* arbitration;
    const char* note; // appended to the readback-creation log line
};
constexpr CaptureFacts kCaptureFacts[kSurfaceCount] = {
    {"vel", "render_target_to_shader_resource", "first_in_frame", ""},
    {"depth", "depth_write_to_readable", "last_before_surface",
     " (plane count comes from the desc: a D24S8 or D32S8 depth buffer is two planes, and copying only the "
     "first would silently truncate it)"},
    {"color", "present_back_buffer", "first_in_frame", ""},
    {"normals", "render_target_to_shader_resource", "first_in_frame", ""},
    {"specular", "render_target_to_shader_resource", "first_in_frame", ""},
};

// How well the fence covers a surface's copy.
//   Strong         - the exact recorded command list was seen submitted to the
//                    fenced queue.
//   Weak           - list identity wasn't observable (e.g. under the debug
//                    layer's wrapper), but the queue did submit after
//                    recording. Consistent, not proven.
//   None           - nothing submitted in between; readback is unsafe.
//   ByConstruction - our own list, submitted by us right before the Signal
//                    (colour): nothing to check, which is not the same claim
//                    as Strong.
enum class Coverage : uint8_t
{
    None,
    Weak,
    Strong,
    ByConstruction
};

// One surface's per-slot capture state.
struct SurfaceCapture
{
    ComPtr<ID3D12Resource> readback;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    // Desc the footprints were derived from; compared every frame since
    // footprints are only valid for the exact layout they came from.
    D3D12_RESOURCE_DESC desc{};
    UINT64 bytes = 0;
    UINT subresources = 0;
    int copies = 0;
    bool present = false;
    // Identity of the command list this copy was recorded onto, used only as a
    // key to check submission (never dereferenced; no ref held). Only
    // velocity's is graded today (see NoteSubmission).
    IUnknown* recordedList = nullptr;
    // Submission count on our queue at the moment the copy was recorded, for
    // the ordering-based fallback when list identity is not observable.
    uint64_t queueSubmissionsAtRecord = 0;
    Coverage coverage = Coverage::None;
};

struct Slot
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    SurfaceCapture surfaces[kSurfaceCount];
    UINT64 fenceValue = 0;
    unsigned long long frameIndex = 0;
    int captureIndex = 0;
    SlotState state = SlotState::Free;
};

// Canonical COM identity: QueryInterface(IID_IUnknown) is the only pointer
// guaranteed equal for the same object. Matters here because under the D3D12
// debug layer, ResourceBarrier's command-list pointer and the one passed to
// ExecuteCommandLists are different wrapper objects - raw pointer comparison
// fails. Safe to call: the object is alive at both call sites.
IUnknown* ComIdentity(IUnknown* object)
{
    if (object == nullptr)
    {
        return nullptr;
    }
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || identity == nullptr)
    {
        return object;
    }
    identity->Release(); // we want the address as a key, not a reference
    return identity;
}

// Whether a cached footprint set still matches this resource's layout.
bool SameLayout(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count && a.Layout == b.Layout;
}

std::string DescToString(const D3D12_RESOURCE_DESC& d)
{
    return std::to_string(d.Width) + "x" + std::to_string(d.Height) +
           " fmt=" + std::to_string(static_cast<int>(d.Format)) + " mips=" + std::to_string(d.MipLevels) +
           " arr=" + std::to_string(d.DepthOrArraySize) + " samples=" + std::to_string(d.SampleDesc.Count);
}

ResourceBarrierFn g_originalResourceBarrier = nullptr;
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

std::mutex g_mutex;
Slot g_slots[kSlots];
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_nextFenceValue = 1;

// Fence-coverage tallies (see Coverage), summarised once per burst.
std::atomic<uint64_t> g_coverageStrong{0};
std::atomic<uint64_t> g_coverageWeak{0};
std::atomic<uint64_t> g_coverageNone{0};
// Colour's: no fence check was needed at all, so not counted as strong.
std::atomic<uint64_t> g_coverageByConstruction{0};
std::atomic<uint64_t> g_queueSubmissions{0};

std::atomic<int> g_framesRemaining{0};

// Hard cap on Presents a burst may span. g_framesRemaining alone counts
// frames submitted, so a burst that never lands the requested count would
// otherwise run forever with F8 dead for the rest of the session.
constexpr int kBurstPresentBudget = kCaptureFrames * 8;
std::atomic<int> g_burstPresentsLeft{0};

// Armed by the hotkey; begins at the next Present, so the first captured
// frame is one whose recording was watched from the start.
std::atomic<bool> g_burstArmed{false};

// Whether depth is required for THIS burst: frozen when it is armed, not
// re-read every frame. Read live, a mid-burst depth reopen would stop requiring
// depth the moment identification went null - a dump mixing frames with and
// without depth that still passes its own completeness check. Frozen, the
// burst ends instead (NotifySurfaceReopened).
std::atomic<bool> g_depthRequiredForBurst{false};

// Arms the burst for the next Present.
void ArmBurst()
{
    g_burstArmed.store(true, std::memory_order_relaxed);
}

// Why a burst's g_framesRemaining reached zero. Set by compare_exchange
// against NotEnded, so whichever condition fires first is what's recorded.
enum class BurstEndReason : uint8_t
{
    NotEnded,
    Completed,                // reached the requested frame count
    LayoutChanged,             // a surface's layout changed mid-dump
    VelocityPassSilent,        // the velocity pass stopped rendering
    PresentBudgetExhausted,    // kBurstPresentBudget Presents spent
    // A surface required for this burst reopened mid-burst: ending beats
    // writing frames captured against a different resource (or none).
    SurfaceReopened,
};
std::atomic<BurstEndReason> g_burstEndReason{BurstEndReason::NotEnded};

// Zeroes g_framesRemaining and records why, if nothing already has. Returns
// how many requested frames were never captured.
int EndBurst(BurstEndReason reason)
{
    BurstEndReason expected = BurstEndReason::NotEnded;
    g_burstEndReason.compare_exchange_strong(expected, reason, std::memory_order_relaxed);
    return g_framesRemaining.exchange(0, std::memory_order_relaxed);
}

// True once nothing from the current (or last) burst is in flight: no frames
// left to record AND no slot waiting on a fence - the signal that the dump
// directory is safe to seal. Caller must hold g_mutex.
bool DumpFullyDrained()
{
    if (g_framesRemaining.load(std::memory_order_relaxed) > 0)
    {
        return false;
    }
    for (const Slot& slot : g_slots)
    {
        if (slot.state == SlotState::Submitted)
        {
            return false;
        }
    }
    return true;
}

// Set when a burst is armed, cleared once the manifest for it is enqueued. Two
// bursts into one MV_DUMP_DIR share it: the manifest describes the directory.
std::atomic<bool> g_manifestDirty{false};

// Frames skipped because every ring slot was in flight. Diagnostic only -
// distinguishes "ring busy as designed" from "ring wedged".
std::atomic<uint64_t> g_noFreeSlot{0};
std::atomic<unsigned long long> g_frameIndex{0};
std::atomic<int> g_captureCounter{0};
std::atomic<bool> g_metadataWritten{false};

// F8 hotkey thread, joinable with a stop flag: a detached thread looping
// forever crashes when FreeLibrary unmaps the module under it (never in a
// game, which never unloads; testhost tears down on every run).
std::atomic<bool> g_hotkeyStop{false};
std::thread g_hotkeyThread;

std::atomic<uint64_t> g_depthEdgeReports{0};
std::atomic<bool> g_depthAfterVelocityReported{false};
// Frames where depth arrived but velocity never did.
std::atomic<uint64_t> g_framesWithoutVelocity{0};
std::atomic<uint64_t> g_incompleteFrames{0};

// ---------------------------------------------------------------------------
// One dump, one layout.
//
// manifest.json describes one layout per surface (layouts[0]), so a
// resolution change mid-dump has nowhere to be recorded. Continuing would
// write frames every offline tool un-pads at the wrong row pitch - a
// plausible image, not an error (the same class of bug as decoding velocity
// as linear). So a layout change ends the burst instead.
struct DumpLayout
{
    D3D12_RESOURCE_DESC desc{};
    bool known = false;
    // Snapshotted the first time this surface is established for the dump.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    UINT64 bytes = 0;
    UINT subresources = 0;
};

DumpLayout g_dumpLayout[kSurfaceCount];

// Records the derived layout the first time a surface's readback is
// established for this dump. Caller must hold g_mutex.
void SnapshotDumpLayout(int surface, const SurfaceCapture& capture)
{
    DumpLayout& tracked = g_dumpLayout[surface];
    if (!tracked.footprints.empty())
    {
        return;
    }
    tracked.footprints = capture.footprints;
    tracked.bytes = capture.bytes;
    tracked.subresources = capture.subresources;
}

// Caller must hold g_mutex. Records the layout the first time a surface is
// seen; afterwards returns false - having already ended the burst - if it has
// changed. Not reset per burst: bursts into one directory share a manifest.
bool DumpLayoutAccepts(int surface, const D3D12_RESOURCE_DESC& desc)
{
    const char* name = kSurfaceNames[surface];
    DumpLayout& tracked = g_dumpLayout[surface];
    if (!tracked.known)
    {
        tracked.desc = desc;
        tracked.known = true;
        return true;
    }
    if (SameLayout(tracked.desc, desc))
    {
        return true;
    }
    const int short_by = EndBurst(BurstEndReason::LayoutChanged);
    Log(std::string("capture: burst ENDED - the ") + name + " layout changed, " + DescToString(tracked.desc) + " -> " +
        DescToString(desc) + ". This dump's manifest.json describes the first layout and has nowhere to record "
        "a second, so continuing would write frames that every offline tool un-pads at the wrong row pitch - "
        "producing a plausible image rather than an error. " +
        std::to_string(short_by) +
        " of the requested frames were not captured. Press F8 again with a FRESH MV_DUMP_DIR to capture at the new "
        "layout; re-using this one would mix the two.");
    return false;
}

// ---------------------------------------------------------------------------
// Background writer. Disk I/O never happens on the render thread - a
// synchronous per-frame write there is a stall that can expose latent bugs in
// the game (DEBUGGING.md). The render thread only copies out and hands off.
// ---------------------------------------------------------------------------

struct WriteJob
{
    std::string path;
    std::vector<uint8_t> data;
    bool append = false;
    // Manifest only: `path` is a .tmp file, renamed over this after a full
    // write (MoveFileExA: a reader sees the old file or the new one, never torn).
    std::string renameTo;
};

// Backpressure cap. An unbounded queue never stalls the render thread but
// risks an unbounded memory spike if disk can't keep up with capture rate.
// Drop instead: a short capture is recoverable, OOM elsewhere isn't. Drops
// are counted and logged so offline tools see why indices have gaps.
constexpr size_t kMaxQueuedBytes = 512ull * 1024 * 1024;

std::mutex g_writeMutex;
std::condition_variable g_writeCv;
std::deque<WriteJob> g_writeQueue;
size_t g_queuedBytes = 0;
bool g_writerStarted = false;
bool g_writerStop = false;
std::thread g_writerThread;
std::atomic<uint64_t> g_droppedWrites{0};
std::atomic<uint64_t> g_failedWrites{0}; // writer thread: open/write errors (disk full)
std::atomic<uint64_t> g_droppedFrames{0};
// Frames that reached the write queue (g_captureCounter counts slots
// submitted to the GPU). The manifest's burst.drained_frames.
std::atomic<uint64_t> g_drainedFrames{0};

// Resolved and created once: it is called per blob, on the render thread.
// MV_DUMP_DIR overrides it, so testhost's synthetic frames never overwrite a
// real capture.
const std::string& DumpDir()
{
    static const std::string dir = []
    {
        char override[MAX_PATH]{};
        if (GetEnvironmentVariableA("MV_DUMP_DIR", override, MAX_PATH) == 0)
        {
            OutputDir("dumps");
            return OutputDir("dumps/mv_dump");
        }
        std::string d = override;
        if (!d.empty() && d.back() != '\\' && d.back() != '/')
        {
            d += '\\';
        }
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

// MV_CAPTURE_DEPTH=0 disables the depth copy for a session - a bandwidth
// isolation test, not a feature. Read once: it is consulted on the hot path,
// and a mid-session change would make frames unlike each other.
bool DepthCaptureEnabled()
{
    static const bool enabled = []
    {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("MV_CAPTURE_DEPTH", value, sizeof(value));
        if (length == 0 || length >= sizeof(value))
        {
            return true;
        }
        const bool off = std::string(value, length) == "0";
        if (off)
        {
            Log("capture: MV_CAPTURE_DEPTH=0 - scene depth will NOT be copied this session. The dump "
                "will have no depth_*.bin and manifest.json will report depth as not present. This is "
                "the bandwidth control test, not a normal capture.");
        }
        return !off;
    }();
    return enabled;
}

// Writes one job; false (logged, counted) if the file could not be opened or
// fully written. A manifest is only renamed into place after a full write.
bool WriteJobToDisk(const WriteJob& job)
{
    HANDLE file = CreateFileA(
        job.path.c_str(),
        job.append ? FILE_APPEND_DATA : GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        job.append ? OPEN_ALWAYS : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    DWORD written = 0;
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok)
    {
        if (job.append)
        {
            SetFilePointer(file, 0, nullptr, FILE_END);
        }
        ok = WriteFile(file, job.data.data(), static_cast<DWORD>(job.data.size()), &written, nullptr) &&
             written == job.data.size();
        CloseHandle(file);
    }
    if (!ok)
    {
        const uint64_t n = g_failedWrites.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5 || n % 100 == 0)
        {
            Log("capture: WRITE FAILED for " + job.path + " (" + std::to_string(written) + " of " +
                std::to_string(job.data.size()) + " bytes, err=" + std::to_string(GetLastError()) + ") - " +
                std::to_string(n) + " failed so far. Disk full, or an unwritable MV_DUMP_DIR?");
        }
        return false;
    }
    if (!job.renameTo.empty() && !MoveFileExA(job.path.c_str(), job.renameTo.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        Log("capture: failed to seal " + job.renameTo + " (rename from " + job.path +
            ") err=" + std::to_string(GetLastError()) +
            " - the dump's blobs are all on disk, but nothing names them; delete the stray .tmp "
            "file and re-seal by pressing F8 again, or note this session's manifest is missing");
    }
    return true;
}

void WriterThread()
{
    for (;;)
    {
        WriteJob job;
        {
            std::unique_lock<std::mutex> lock(g_writeMutex);
            g_writeCv.wait(lock, [] { return !g_writeQueue.empty() || g_writerStop; });
            if (g_writeQueue.empty())
            {
                return; // stopping, and everything queued has been written
            }
            job = std::move(g_writeQueue.front());
            g_writeQueue.pop_front();
            g_queuedBytes -= job.data.size();
        }
        WriteJobToDisk(job);

        // Confirms everything handed over so far is actually on disk - this,
        // not "burst recorded", is the signal it's safe to quit.
        std::lock_guard<std::mutex> lock(g_writeMutex);
        if (g_writeQueue.empty())
        {
            const uint64_t failed = g_failedWrites.load(std::memory_order_relaxed);
            Log(failed == 0 ? std::string("capture: write queue drained - all captured frames are on disk")
                            : "capture: write queue drained, but " + std::to_string(failed) +
                                  " write(s) FAILED - see the WRITE FAILED lines");
        }
    }
}

// Is there room for `bytes` more of queued output? Checked first so a frame is
// dropped whole, never as a dump missing one surface for some frames.
bool WriteQueueHasRoom(size_t bytes)
{
    std::lock_guard<std::mutex> lock(g_writeMutex);
    return g_queuedBytes + bytes <= kMaxQueuedBytes;
}

// Returns false if the job was dropped for backpressure. A manifest write
// (`renameTo` non-empty) always goes through.
bool EnqueueWrite(std::string path, std::vector<uint8_t> data, bool append = false, std::string renameTo = {})
{
    {
        std::lock_guard<std::mutex> lock(g_writeMutex);
        if (g_writerStop)
        {
            return false;
        }
        if (!g_writerStarted)
        {
            g_writerStarted = true;
            g_writerThread = std::thread(WriterThread);
        }
        // Small bookkeeping writes (frames.csv, manifest) always go through -
        // without them the frames that did land cannot be interpreted.
        if (renameTo.empty() && data.size() > 4096 && g_queuedBytes + data.size() > kMaxQueuedBytes)
        {
            const uint64_t n = g_droppedWrites.fetch_add(1) + 1;
            Log("capture: WRITE QUEUE FULL (" + std::to_string(g_queuedBytes / (1024 * 1024)) +
                "MB pending), dropping " + path + " - dropped " + std::to_string(n) + " so far");
            return false;
        }
        g_queuedBytes += data.size();
        g_writeQueue.push_back(WriteJob{std::move(path), std::move(data), append, std::move(renameTo)});
    }
    g_writeCv.notify_one();
    return true;
}

// Enqueues all of one frame's blobs, plus its frames.csv line, under one hold
// of g_writeMutex: all of them or none. Returns false if the frame was dropped
// for backpressure (DrainSlot checks room first, so this should not happen).
bool EnqueueFrame(std::vector<WriteJob> jobs)
{
    size_t total = 0;
    for (const WriteJob& job : jobs)
    {
        total += job.data.size();
    }
    {
        std::lock_guard<std::mutex> lock(g_writeMutex);
        if (g_writerStop)
        {
            return false;
        }
        if (!g_writerStarted)
        {
            g_writerStarted = true;
            g_writerThread = std::thread(WriterThread);
        }
        if (g_queuedBytes + total > kMaxQueuedBytes)
        {
            const uint64_t n = g_droppedWrites.fetch_add(jobs.size()) + jobs.size();
            Log("capture: WRITE QUEUE FULL (" + std::to_string(g_queuedBytes / (1024 * 1024)) +
                "MB pending), dropping " + std::to_string(jobs.size()) + " blob(s) for one frame (" +
                std::to_string(total / (1024 * 1024)) + "MB) - dropped " + std::to_string(n) + " so far");
            return false;
        }
        g_queuedBytes += total;
        for (WriteJob& job : jobs)
        {
            g_writeQueue.push_back(std::move(job));
        }
    }
    g_writeCv.notify_one();
    return true;
}

// ---------------------------------------------------------------------------

// Creates a READBACK-heap resource of the given size.
bool CreateReadbackBuffer(ID3D12Device* device, UINT64 bytes, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
    return SUCCEEDED(hr);
}

// Plane count for a format (depth/stencil = 2, most others = 1). Subresource
// count is mips * arraySize * planeCount - getting this wrong silently
// truncates the copy.
UINT PlaneCountFor(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_INFO info{};
    info.Format = format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))))
    {
        return 1;
    }
    return info.PlaneCount == 0 ? 1 : info.PlaneCount;
}

UINT SubresourceCountFor(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc)
{
    const UINT mips = desc.MipLevels == 0 ? 1 : desc.MipLevels;
    const UINT slices = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                            ? 1
                            : (desc.DepthOrArraySize == 0 ? 1 : desc.DepthOrArraySize);
    return mips * slices * PlaneCountFor(device, desc.Format);
}

// Makes sure a surface has a readback buffer matching `desc`, rebuilding its
// footprints if the layout changed or this is the slot's first use. Compared
// every frame: resolution can change mid-session (dynamic resolution), and
// stale footprints would corrupt the copy into plausible-looking garbage.
// Returns false if the buffer could not be created. Caller must hold g_mutex.
bool EnsureReadback(SurfaceCapture& capture, int surface, const D3D12_RESOURCE_DESC& desc)
{
    if (capture.readback != nullptr && SameLayout(capture.desc, desc))
    {
        return true;
    }
    const char* name = kSurfaceNames[surface];
    if (capture.readback != nullptr)
    {
        Log(std::string("capture: ") + name + " layout changed, " + DescToString(capture.desc) + " -> " +
            DescToString(desc) + "; rebuilding footprints");
    }
    const UINT subresources = SubresourceCountFor(g_device.Get(), desc);
    capture.footprints.resize(subresources);
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&desc, 0, subresources, 0, capture.footprints.data(), nullptr, nullptr, &total);
    capture.bytes = total;
    capture.subresources = subresources;
    capture.desc = desc;
    capture.readback.Reset();
    if (!CreateReadbackBuffer(g_device.Get(), total, capture.readback))
    {
        Log(std::string("capture: failed to create ") + name + " readback buffer");
        return false;
    }
    Log(std::string("capture: ") + name + " readback buffer created, " + std::to_string(total) + " bytes, " +
        std::to_string(subresources) +
        " subresource(s), rowPitch=" + std::to_string(capture.footprints[0].Footprint.RowPitch) + kCaptureFacts[surface].note);
    return true;
}

// Copies every subresource of a texture into a READBACK buffer, bracketed by
// barriers that restore the resource's original state afterward.
//
// Footprints are laid out back-to-back in subresource order (as
// GetCopyableFootprints described them) so offline tools can address any
// subresource from one file. Derived from the desc rather than assumed, so
// mipped/arrayed/planar resources (e.g. depth/stencil) are handled correctly
// rather than truncated to their first plane.
void RecordCopyToReadback(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* source,
    ID3D12Resource* dest,
    const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& footprints,
    D3D12_RESOURCE_STATES stateBefore,
    ResourceBarrierFn barrierFn)
{
    if (footprints.empty())
    {
        return;
    }

    // ALL_SUBRESOURCES transitions every subresource in one barrier - cheaper,
    // and correct for multi-plane resources that would otherwise need one each.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = source;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrierFn(cmdList, 1, &barrier);

    for (UINT i = 0; i < static_cast<UINT>(footprints.size()); ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = dest;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprints[i];

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = source;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = i;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Restore - leaving COPY_SOURCE would desync the game's own barrier
    // bookkeeping and trip the debug layer / corrupt rendering.
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = stateBefore;
    barrierFn(cmdList, 1, &barrier);
}

// MV_ENGINE_VERSION=<major>.<minor>, for the manifest's "engine" block. Unset
// stays unset rather than guessed: 5.7 packs velocity channel 3 differently
// from 5.2, so a wrong guess mis-decodes silently.
bool ParseEngineVersion(int* major, int* minor)
{
    char version[32]{};
    const DWORD length = GetEnvironmentVariableA("MV_ENGINE_VERSION", version, sizeof(version));
    if (length == 0 || length >= sizeof(version))
    {
        return false;
    }
    const std::string value(version, length);
    const size_t dot = value.find('.');
    if (dot == std::string::npos)
    {
        Log("capture: MV_ENGINE_VERSION='" + value +
            "' is not in major.minor form; not recording it rather than recording a guess");
        return false;
    }
    *major = atoi(value.c_str());
    *minor = atoi(value.c_str() + dot + 1);
    return true;
}

// Truncates frames.csv to its header, so a new session never appends onto a
// previous one's capture indices.
void TruncateFramesCsv()
{
    const std::string header = "# captureIndex,gameFrameIndex\n";
    EnqueueWrite(DumpDir() + "frames.csv", std::vector<uint8_t>(header.begin(), header.end()));
}

// ---------------------------------------------------------------------------
// manifest.json (schema in docs/REFACTOR_PLAN.md sec 4.1). Depth's layout lists
// both planes: D24S8/D32S8 keep depth in plane 0 and stencil in plane 1, with
// different formats and row pitches.

std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string JsonString(const std::string& s)
{
    return "\"" + JsonEscape(s) + "\"";
}

// One plane's footprint, as a manifest object literal. Depth is two planes
// (depth + stencil); velocity and colour are one.
std::string PlaneJson(int index, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f, int indent)
{
    const std::string pad(indent, ' ');
    return pad + "{ \"index\": " + std::to_string(index) + ", \"offset\": " + std::to_string(f.Offset) +
           ", \"width\": " + std::to_string(f.Footprint.Width) + ", \"height\": " +
           std::to_string(f.Footprint.Height) + ", \"row_pitch\": " + std::to_string(f.Footprint.RowPitch) +
           ", \"format\": " + std::to_string(static_cast<int>(f.Footprint.Format)) + " }";
}

std::string LayoutSurfaceJson(const DumpLayout& layout, int indent)
{
    const std::string pad(indent, ' ');
    std::string out = pad + "{\n";
    out += pad + "  \"format\": " + std::to_string(static_cast<int>(layout.desc.Format)) + ",\n";
    out += pad + "  \"bytes\": " + std::to_string(layout.bytes) + ",\n";
    out += pad + "  \"subresources\": " + std::to_string(layout.subresources) + ",\n";
    out += pad + "  \"planes\": [\n";
    for (size_t i = 0; i < layout.footprints.size(); ++i)
    {
        out += PlaneJson(static_cast<int>(i), layout.footprints[i], indent + 4);
        out += (i + 1 < layout.footprints.size()) ? ",\n" : "\n";
    }
    out += pad + "  ]\n";
    out += pad + "}";
    return out;
}

std::string ProcessBaseName()
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* base = path;
    for (const char* p = path; *p; ++p)
    {
        if (*p == '\\' || *p == '/')
        {
            base = p + 1;
        }
    }
    return base;
}

std::string UtcNowIso8601()
{
    SYSTEMTIME t{};
    GetSystemTime(&t);
    char buf[32]{};
    sprintf_s(
        buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

// When the first burst into this dump directory started.
std::string g_dumpStartedUtc;

// Assembles manifest.json's full content. Caller must hold g_mutex (reads
// g_dumpLayout, g_burstEndReason, and friends without their own locking).
std::string BuildManifest()
{
    std::string j;
    j += "{\n";
    j += "  \"schema_version\": 1,\n";
    j += "  \"producer\": { \"tool\": \"mv_hook\", \"commit\": " + JsonString(kBuildGitCommit) +
         ", \"configured_utc\": " + JsonString(kBuildConfiguredUtc) + " },\n";

    const std::string process = ProcessBaseName();
    const std::string sealedUtc = UtcNowIso8601();
    j += "  \"session\": {\n";
    j += "    \"process\": " + JsonString(process) + ",\n";
    j += "    \"started_utc\": " + JsonString(g_dumpStartedUtc) + ",\n";
    j += "    \"sealed_utc\": " + JsonString(sealedUtc) + ",\n";
    // "embedded" means the DLL's own profiles/ue5.json, which producer.commit
    // pins; anything else is an MV_PROFILE_FILE path, which it does not.
    if (const SurfaceProfile* profile = ActiveSurfaceProfile())
    {
        j += "    \"profile\": " + JsonString(profile->name) + ",\n";
        j += "    \"profile_source\": " + JsonString(profile->source) + ",\n";
    }
    else
    {
        j += "    \"profile\": null,\n";
        j += "    \"profile_source\": null,\n";
    }
    if (g_dumpLayout[kSurfaceColor].known)
    {
        const D3D12_RESOURCE_DESC& bb = g_dumpLayout[kSurfaceColor].desc;
        j += "    \"back_buffer\": { \"width\": " + std::to_string(bb.Width) + ", \"height\": " +
             std::to_string(bb.Height) + ", \"format\": " + std::to_string(static_cast<int>(bb.Format)) + " }\n";
    }
    else
    {
        j += "    \"back_buffer\": null\n";
    }
    j += "  },\n";

    int engineMajor = 0;
    int engineMinor = 0;
    if (ParseEngineVersion(&engineMajor, &engineMinor))
    {
        j += "  \"engine\": { \"major\": " + std::to_string(engineMajor) + ", \"minor\": " +
             std::to_string(engineMinor) + ", \"source\": \"MV_ENGINE_VERSION\" },\n";
    }
    else
    {
        j += "  \"engine\": null,\n";
    }

    static const char* const kReasonNames[] = {
        "not_ended", "completed", "layout_changed", "velocity_pass_silent", "present_budget_exhausted",
        "surface_reopened"};
    const BurstEndReason reason = g_burstEndReason.load(std::memory_order_relaxed);
    j += "  \"burst\": {\n";
    j += "    \"requested_frames\": " + std::to_string(kCaptureFrames) + ",\n";
    j += "    \"recorded_frames\": " + std::to_string(g_captureCounter.load(std::memory_order_relaxed)) + ",\n";
    j += "    \"drained_frames\": " + std::to_string(g_drainedFrames.load(std::memory_order_relaxed)) + ",\n";
    j += "    \"ended_because\": " + JsonString(kReasonNames[static_cast<int>(reason)]) + "\n";
    j += "  },\n";

    j += "  \"surfaces\": {\n";
    for (int i = 0; i < kSurfaceCount; ++i)
    {
        const DumpLayout& layout = g_dumpLayout[i];
        const CaptureFacts& facts = kCaptureFacts[i];
        j += std::string("    ") + JsonString(kSurfaceNames[i]) + ": {\n";
        j += "      \"present\": " + std::string(layout.known ? "true" : "false") + ",\n";
        if (!layout.known && i == kSurfaceDepth)
        {
            j += std::string("      \"reason\": ") +
                 (DepthCaptureEnabled() ? "\"never_identified\"" : "\"capture_disabled\"") + ",\n";
        }
        if (!layout.known && i >= kFirstOptionalSurface)
        {
            // So a consumer can tell "off" from "on but never found".
            const std::string name = kSurfaceNames[i];
            const SurfaceProfile* profile = ActiveSurfaceProfile();
            const SurfaceSpec* spec = profile != nullptr ? profile->Optional(name.c_str()) : nullptr;
            const bool enabled = spec != nullptr && spec->enabled;
            j += std::string("      \"reason\": ") + (enabled ? "\"never_identified\"" : "\"not_enabled\"") + ",\n";
            j += "      \"detail\": " +
                 JsonString(enabled ? "enabled via MV_SURFACES=" + name + ", but no resource was identified - see the '" +
                                          name + ":' lines in mv_hook.log"
                            : spec != nullptr ? "spec exists in profile " + profile->name +
                                                    " but enabled=false; no identification was attempted"
                                              : "the active profile has no " + name + " spec") +
                 ",\n";
        }
        const bool required = i == kSurfaceVelocity || i == kSurfaceColor || (i == kSurfaceDepth && layout.known);
        j += "      \"required\": " + std::string(required ? "true" : "false") + ",\n";
        if (i == kSurfaceColor)
        {
            j += "      \"source\": { \"kind\": \"known_by_construction\", \"mechanism\": "
                 "\"IDXGISwapChain3::GetBuffer(GetCurrentBackBufferIndex())\" },\n";
        }
        else
        {
            j += "      \"source\": { \"kind\": \"identified\" },\n";
        }
        j += std::string("      \"trigger\": ") + JsonString(facts.trigger) + ",\n";
        j += std::string("      \"arbitration\": ") + JsonString(facts.arbitration) + ",\n";
        j += std::string("      \"file_pattern\": ") + JsonString(std::string(facts.filePrefix) + "_%05d.bin");
        if (i >= kFirstOptionalSurface)
        {
            // No optional surface's encoding is verified against engine
            // source; "unknown" is what makes mvtools.py refuse to decode it.
            j += ",\n      \"encoding\": { \"kind\": \"unknown\" }";
        }
        if (i == kSurfaceVelocity)
        {
            j += ",\n      \"coverage\": { \"strong\": " +
                 std::to_string(g_coverageStrong.load(std::memory_order_relaxed)) + ", \"weak\": " +
                 std::to_string(g_coverageWeak.load(std::memory_order_relaxed)) + ", \"none\": " +
                 std::to_string(g_coverageNone.load(std::memory_order_relaxed)) + " }\n";
        }
        else if (i == kSurfaceColor)
        {
            j += ",\n      \"coverage\": { \"by_construction\": " +
                 std::to_string(g_coverageByConstruction.load(std::memory_order_relaxed)) + " }\n";
        }
        else
        {
            // Rides the game's list like velocity's, but its fence coverage is
            // not graded - say so rather than report zeros.
            j += ",\n      \"coverage_note\": \"not evaluated - see docs/REFACTOR_PLAN.md finding 1.2\"\n";
        }
        j += std::string("    }") + (i + 1 < kSurfaceCount ? ",\n" : "\n");
    }
    j += "  },\n";

    j += "  \"layouts\": [\n";
    j += "    {\n";
    j += "      \"epoch\": 0,\n";
    j += "      \"surfaces\": {\n";
    bool wroteAny = false;
    std::string layoutSurfaces;
    for (int i = 0; i < kSurfaceCount; ++i)
    {
        if (!g_dumpLayout[i].known)
        {
            continue;
        }
        if (wroteAny)
        {
            layoutSurfaces += ",\n";
        }
        layoutSurfaces += "        " + JsonString(kSurfaceNames[i]) + ": " + LayoutSurfaceJson(g_dumpLayout[i], 8);
        wroteAny = true;
    }
    j += layoutSurfaces;
    j += wroteAny ? "\n" : "";
    j += "      }\n";
    j += "    }\n";
    j += "  ],\n";

    j += "  \"frames\": {\n";
    j += "    \"file\": \"frames.csv\",\n";
    j += "    \"columns\": [\"capture_index\", \"game_frame_index\"],\n";
    j += "    \"note\": \"wider columns (layout_epoch, surfaces_mask, coverage) arrive in Phase 5; this "
         "dump's frames.csv is the 2-column format\"\n";
    j += "  },\n";

    j += "  \"integrity\": {\n";
    j += "    \"sealed\": true,\n";
    j += "    \"required_surfaces\": [\"velocity\", \"color\"";
    if (g_dumpLayout[kSurfaceDepth].known)
    {
        j += ", \"depth\"";
    }
    j += "],\n";
    j += "    \"dropped_frames_backpressure\": " + std::to_string(g_droppedFrames.load(std::memory_order_relaxed)) +
         ",\n";
    j += "    \"frames_without_velocity\": " +
         std::to_string(g_framesWithoutVelocity.load(std::memory_order_relaxed)) + ",\n";
    j += "    \"incomplete_frames_dropped\": " +
         std::to_string(g_incompleteFrames.load(std::memory_order_relaxed)) + ",\n";
    j += "    \"slots_missed_busy\": " + std::to_string(g_noFreeSlot.load(std::memory_order_relaxed)) + ",\n";
    j += "    \"writes_dropped\": " + std::to_string(g_droppedWrites.load(std::memory_order_relaxed)) + "\n";
    j += "  }\n";

    j += "}\n";
    return j;
}

// Enqueues the manifest as the LAST job after every blob it names: the writer
// is FIFO, so every blob is on disk before the manifest appears.
void WriteManifestNow()
{
    const std::string json = BuildManifest();
    EnqueueWrite(
        DumpDir() + "manifest.json.tmp",
        std::vector<uint8_t>(json.begin(), json.end()),
        /*append=*/false,
        /*renameTo=*/DumpDir() + "manifest.json");
}

// Maps one finished readback buffer out to a vector.
bool MapOut(ID3D12Resource* buffer, UINT64 bytes, std::vector<uint8_t>& out)
{
    if (buffer == nullptr || bytes == 0)
    {
        return false;
    }
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(bytes)};
    if (FAILED(buffer->Map(0, &readRange, &mapped)) || mapped == nullptr)
    {
        return false;
    }
    out.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + bytes);
    D3D12_RANGE noWrite{0, 0};
    buffer->Unmap(0, &noWrite);
    return true;
}

// Maps a finished slot's readback buffers and hands their bytes to the writer,
// all of the frame or none of it.
bool DrainSlot(Slot& slot)
{
    // Room is checked BEFORE any Map(): the sizes are known from the
    // footprints, so a dropped frame costs no copy out of GPU memory.
    size_t total = 0;
    for (int i = 0; i < kSurfaceCount; ++i)
    {
        if (slot.surfaces[i].present)
        {
            total += slot.surfaces[i].bytes;
        }
    }
    if (!WriteQueueHasRoom(total))
    {
        const uint64_t n = g_droppedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        Log("capture: write queue has no room for frame " + std::to_string(slot.captureIndex) + " (" +
            std::to_string(total / (1024 * 1024)) + "MB) - dropping the WHOLE frame BEFORE mapping anything, " +
            std::to_string(n) +
            " so far. A partial frame would be worse: nothing in the dump distinguishes 'depth was "
            "dropped here' from 'depth was never captured at all'.");
        return false;
    }

    // Velocity and the back buffer are what every downstream tool keys off, so
    // a slot without both is not a capture.
    std::vector<uint8_t> data[kSurfaceCount];
    bool mapped[kSurfaceCount] = {};
    if (!MapOut(
            slot.surfaces[kSurfaceVelocity].readback.Get(),
            slot.surfaces[kSurfaceVelocity].bytes,
            data[kSurfaceVelocity]) ||
        !MapOut(slot.surfaces[kSurfaceColor].readback.Get(), slot.surfaces[kSurfaceColor].bytes, data[kSurfaceColor]))
    {
        return false;
    }
    mapped[kSurfaceVelocity] = true;
    mapped[kSurfaceColor] = true;

    for (int i = 0; i < kSurfaceCount; ++i)
    {
        if (mapped[i] || !slot.surfaces[i].present)
        {
            continue;
        }
        mapped[i] = MapOut(slot.surfaces[i].readback.Get(), slot.surfaces[i].bytes, data[i]);
    }

    char nameBuf[64]{};
    std::vector<WriteJob> jobs;
    jobs.reserve(kSurfaceCount + 1);
    bool haveVelocity = false;
    bool haveColor = false;
    for (int i = 0; i < kSurfaceCount; ++i)
    {
        if (!mapped[i])
        {
            continue;
        }
        sprintf_s(nameBuf, "%s_%05d.bin", kCaptureFacts[i].filePrefix, slot.captureIndex);
        jobs.push_back(WriteJob{DumpDir() + nameBuf, std::move(data[i]), /*append=*/false, /*renameTo=*/{}});
        haveVelocity = haveVelocity || i == kSurfaceVelocity;
        haveColor = haveColor || i == kSurfaceColor;
    }

    // Which GAME frame this capture is: capture indices only count what was
    // drained, and warp validation needs N-1/N to be one frame apart.
    if (haveVelocity && haveColor)
    {
        char line[64]{};
        sprintf_s(line, "%d,%llu\n", slot.captureIndex, slot.frameIndex);
        jobs.push_back(WriteJob{DumpDir() + "frames.csv", std::vector<uint8_t>(line, line + strlen(line)),
                                 /*append=*/true, /*renameTo=*/{}});
    }

    const bool ok = EnqueueFrame(std::move(jobs)) && haveVelocity && haveColor;
    if (ok)
    {
        g_drainedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

} // namespace

void SetOriginalFunctions(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists)
{
    g_originalResourceBarrier = resourceBarrier;
    g_originalExecuteCommandLists = executeCommandLists;
}

void NoteCommandQueue(ID3D12CommandQueue* queue)
{
    if (queue == nullptr || g_queue != nullptr)
    {
        return;
    }
    // Only the DIRECT queue is useful: the back-buffer copy must land on the
    // queue the game renders with, so one fence covers both it and velocity.
    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue == nullptr)
    {
        g_queue = queue;
        Log("capture: noted direct command queue");
    }
}

ID3D12CommandQueue* GetGameQueue()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_queue.Get();
}

void SetFrameIndex(unsigned long long frameIndex)
{
    g_frameIndex.store(frameIndex, std::memory_order_relaxed);
}

void StartCaptureHotkeyThread()
{
    if (g_hotkeyThread.joinable())
    {
        return;
    }
    g_hotkeyThread = std::thread(
        []
        {
            // MV_AUTOCAPTURE: for mv_testhost, which has no one to press F8.
            if (GetEnvironmentVariableA("MV_AUTOCAPTURE", nullptr, 0) != 0)
            {
                Sleep(1000);
                ArmBurst();
                Log("capture: MV_AUTOCAPTURE set, starting a burst without a keypress");
            }
            bool wasDown = false;
            while (!g_hotkeyStop.load(std::memory_order_relaxed))
            {
                const bool isDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
                if (isDown && !wasDown && g_framesRemaining.load() <= 0)
                {
                    ArmBurst();
                    Log("capture: F8 pressed, capturing " + std::to_string(kCaptureFrames) +
                        " frames from the next frame boundary");
                }
                wasDown = isDown;
                Sleep(30);
            }
        });
    Log("capture: hotkey thread started (F8 = capture burst)");
}

namespace
{

// Caller must hold g_mutex.
bool EnsureDevice(ID3D12GraphicsCommandList* cmdList)
{
    if (g_device != nullptr)
    {
        return true;
    }
    if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&g_device))) || g_device == nullptr)
    {
        return false;
    }
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
    {
        Log("capture: CreateFence failed");
        g_device.Reset();
        return false;
    }
    return true;
}

// The slot this frame's copies go into: the one already recording this
// frame, or a free one - whichever surface arrives first allocates. Caller must
// hold g_mutex.
Slot* AcquireSlotForFrame(unsigned long long frame)
{
    for (Slot& candidate : g_slots)
    {
        if (candidate.state == SlotState::Recording && candidate.frameIndex == frame)
        {
            return &candidate;
        }
    }
    for (Slot& candidate : g_slots)
    {
        if (candidate.state == SlotState::Free)
        {
            candidate.frameIndex = frame;
            candidate.state = SlotState::Recording;
            // The readback buffers survive - reusing them is the point of the
            // ring; EnsureReadback revalidates the layout before each copy.
            for (SurfaceCapture& surface : candidate.surfaces)
            {
                surface.present = false;
                surface.copies = 0;
                surface.recordedList = nullptr;
                surface.queueSubmissionsAtRecord = 0;
                surface.coverage = Coverage::None;
            }
            return &candidate;
        }
    }
    return nullptr;
}

// All slots in flight: the frame is skipped rather than stalled. Logged, since
// it is otherwise indistinguishable from a wedged ring. Caller holds g_mutex.
void LogNoFreeSlot()
{
    const uint64_t misses = g_noFreeSlot.fetch_add(1, std::memory_order_relaxed) + 1;
    if (misses > 3 && misses % 240 != 0)
    {
        return;
    }
    std::string states;
    for (const Slot& s : g_slots)
    {
        states += (states.empty() ? "" : ",");
        switch (s.state)
        {
            case SlotState::Free:
                states += "free";
                break;
            case SlotState::Recording:
                states += "recording@f" + std::to_string(s.frameIndex) +
                          (s.surfaces[kSurfaceVelocity].present ? "+vel" : "") +
                          (s.surfaces[kSurfaceDepth].present ? "+depth" : "");
                break;
            case SlotState::Submitted:
                states += "submitted@fence" + std::to_string(s.fenceValue);
                break;
        }
    }
    Log("capture: no free slot (" + std::to_string(misses) + " skipped so far, " +
        std::to_string(g_framesRemaining.load(std::memory_order_relaxed)) + " frames still requested) - slots=[" +
        states + "] fenceCompleted=" + std::to_string(g_fence != nullptr ? g_fence->GetCompletedValue() : 0) +
        " nextFence=" + std::to_string(g_nextFenceValue));
}

} // namespace

void OnSurfaceReadable(int surface, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, int stateAfter)
{
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0 || cmdList == nullptr || resource == nullptr ||
        g_originalResourceBarrier == nullptr || surface < 0 || surface >= kSurfaceCount || surface == kSurfaceColor ||
        (surface == kSurfaceDepth && !DepthCaptureEnabled()))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureDevice(cmdList))
    {
        return;
    }
    const unsigned long long frame = g_frameIndex.load(std::memory_order_relaxed);
    Slot* slot = AcquireSlotForFrame(frame);
    if (slot == nullptr)
    {
        if (surface == kSurfaceVelocity)
        {
            LogNoFreeSlot();
        }
        return;
    }
    SurfaceCapture& capture = slot->surfaces[surface];

    // Which of a frame's edges lands (kCaptureFacts arbitration).
    if (surface == kSurfaceDepth)
    {
        // last_before_surface: depth goes readable several times a frame and the
        // first edge can be a partial pass (on Skyrunner, the weapon alone), so
        // keep re-copying until velocity has been copied - the depth the frame's
        // velocity was rendered against. A title whose depth goes readable only
        // AFTER velocity still gets its first edge (copies == 0).
        const bool velocityDone = slot->surfaces[kSurfaceVelocity].present;
        if (capture.copies >= kMaxDepthCopiesPerFrame || (velocityDone && capture.copies > 0))
        {
            return;
        }
        if (velocityDone && !g_depthAfterVelocityReported.exchange(true))
        {
            Log("capture: this title's depth buffer becomes readable only AFTER the velocity buffer "
                "does, so the depth in the dump is from a pass that ran after velocity was finished. "
                "For opaque geometry that is the same depth; if this title writes depth after its "
                "velocity pass, it is not.");
        }
    }
    else if (capture.present)
    {
        return; // first_in_frame: velocity's and the GBuffers' first RT -> SRV edge
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (!DumpLayoutAccepts(surface, desc))
    {
        return;
    }
    const bool firstUse = capture.readback == nullptr;
    if (!EnsureReadback(capture, surface, desc))
    {
        return;
    }
    if (firstUse)
    {
        SnapshotDumpLayout(surface, capture);
    }
    RecordCopyToReadback(cmdList, resource, capture.readback.Get(), capture.footprints,
                         static_cast<D3D12_RESOURCE_STATES>(stateAfter), g_originalResourceBarrier);

    // Recorded on the GAME's list: whether our fence covers it is checked in
    // NoteSubmission, by COM identity (see ComIdentity), not assumed.
    capture.recordedList = ComIdentity(cmdList);
    capture.coverage = Coverage::None;
    capture.queueSubmissionsAtRecord = g_queueSubmissions.load(std::memory_order_relaxed);
    ++capture.copies;
    capture.present = true;

    if (surface == kSurfaceDepth && g_depthEdgeReports.fetch_add(1, std::memory_order_relaxed) < 8)
    {
        Log("capture: depth copy " + std::to_string(capture.copies) + " of frame " + std::to_string(frame) +
            " recorded (stateAfter=" + std::to_string(stateAfter) +
            "). The last copy of a frame is the one that lands in the dump.");
    }
}

int SurfaceIndex(const char* name)
{
    for (int i = 0; name != nullptr && i < kSurfaceCount; ++i)
    {
        if (std::strcmp(kSurfaceNames[i], name) == 0)
        {
            return i;
        }
    }
    return -1;
}

void NoteSubmission(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists)
{
    // Runs for every submission: nothing to check outside a burst.
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0 || commandLists == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (queue == g_queue.Get())
    {
        g_queueSubmissions.fetch_add(1, std::memory_order_relaxed);
    }
    for (Slot& slot : g_slots)
    {
        if (slot.state != SlotState::Recording)
        {
            continue;
        }
        // Velocity only: depth's and the GBuffers' copies also ride the
        // game's list and are not graded yet.
        SurfaceCapture& surface = slot.surfaces[kSurfaceVelocity];
        if (surface.recordedList == nullptr)
        {
            continue;
        }
        for (UINT i = 0; i < numCommandLists; ++i)
        {
            if (ComIdentity(commandLists[i]) != surface.recordedList)
            {
                continue;
            }
            if (queue == g_queue.Get())
            {
                surface.coverage = Coverage::Strong;
            }
            else
            {
                // Correctness rests on one fence covering our copy and the
                // velocity copy, so a second DIRECT queue would read stale.
                Log("capture: WARNING velocity command list submitted on a queue other than the "
                    "one we fence on - readback for frame " +
                    std::to_string(slot.frameIndex) + " is NOT covered by our fence");
            }
        }
    }
}

void NotifySurfaceReopened(const char* surfaceName)
{
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0)
    {
        return; // no burst in progress; nothing to end
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0)
    {
        return; // ended between the check above and taking the lock
    }
    const std::string name = surfaceName != nullptr ? surfaceName : "";
    // Velocity is always required; depth's requirement is frozen per burst
    // (g_depthRequiredForBurst).
    const bool required = name == "velocity" || (name == "depth" && g_depthRequiredForBurst.load(std::memory_order_relaxed));
    if (!required)
    {
        return;
    }
    const int short_by = EndBurst(BurstEndReason::SurfaceReopened);
    Log("capture: burst ENDED - the " + name +
        " search reopened mid-burst, so later frames would have been captured against a different resource (or "
        "none) than earlier ones. " +
        std::to_string(short_by) + " of the requested " + std::to_string(kCaptureFrames) +
        " never landed. Ending rather than silently dropping frames until reidentification completes keeps the "
        "dump homogeneous - press F8 again once the search has settled.");
}

// ---------------------------------------------------------------------------
// MV_SNAPSHOT=<surface>: copies ONE frame of each resource that surface has
// shortlisted at the scene extent, at its own RT -> SRV edge, to
// snapshot_<surface>_<ptr>_<w>x<h>_fmt<f>_pitch<p>.bin in the dump directory -
// for telling candidates apart by their pixels when every barrier signal ties
// them (how GBufferB was found among seven B8G8R8A8_TYPELESS targets). Changes
// no selection. The copy rides the game's list; the fence is signalled on the
// game's queue at a LATER Present, after that list was submitted. Guarded by
// g_mutex.
// ---------------------------------------------------------------------------
namespace
{

struct Snapshot
{
    std::string surface;
    ID3D12Resource* source = nullptr; // identity only (for the filename); not dereferenced after recording
    D3D12_RESOURCE_DESC desc{};
    ComPtr<ID3D12Resource> readback;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    UINT64 bytes = 0;
    unsigned long long frame = 0;
    UINT64 fence = 0;
};
std::vector<Snapshot> g_snapshots;
std::vector<ID3D12Resource*> g_snapshotTaken;
constexpr size_t kMaxSnapshots = 32;

const std::string& SnapshotSurface()
{
    static const std::string surface = []
    {
        char value[64]{};
        const DWORD length = GetEnvironmentVariableA("MV_SNAPSHOT", value, sizeof(value));
        return (length > 0 && length < sizeof(value)) ? std::string(value, length) : std::string();
    }();
    return surface;
}

// Caller must hold g_mutex.
void ProcessSnapshots()
{
    if (g_snapshots.empty() || g_queue == nullptr || g_fence == nullptr)
    {
        return;
    }
    const unsigned long long now = g_frameIndex.load(std::memory_order_relaxed);
    for (auto it = g_snapshots.begin(); it != g_snapshots.end();)
    {
        Snapshot& s = *it;
        if (s.fence == 0)
        {
            if (now > s.frame && SUCCEEDED(g_queue->Signal(g_fence.Get(), g_nextFenceValue)))
            {
                s.fence = g_nextFenceValue++;
            }
            ++it;
            continue;
        }
        if (g_fence->GetCompletedValue() < s.fence)
        {
            ++it;
            continue;
        }
        std::vector<uint8_t> data;
        if (MapOut(s.readback.Get(), s.bytes, data))
        {
            char name[160]{};
            sprintf_s(name, "snapshot_%s_%llu_%llux%u_fmt%d_pitch%u.bin", s.surface.c_str(),
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.source)), s.desc.Width,
                      s.desc.Height, static_cast<int>(s.desc.Format), s.footprints[0].Footprint.RowPitch);
            EnqueueWrite(DumpDir() + name, std::move(data));
            Log(std::string("capture: snapshot written - ") + name + " (copied on frame " + std::to_string(s.frame) +
                ")");
        }
        it = g_snapshots.erase(it);
    }
}

} // namespace

void SnapshotCandidate(const char* surfaceName, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                       int stateAfter)
{
    if (SnapshotSurface().empty() || surfaceName == nullptr || SnapshotSurface() != surfaceName ||
        cmdList == nullptr || resource == nullptr || g_originalResourceBarrier == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_snapshotTaken.size() >= kMaxSnapshots ||
        std::find(g_snapshotTaken.begin(), g_snapshotTaken.end(), resource) != g_snapshotTaken.end() ||
        !EnsureDevice(cmdList))
    {
        return;
    }
    g_snapshotTaken.push_back(resource);

    Snapshot s;
    s.surface = surfaceName;
    s.source = resource;
    s.desc = resource->GetDesc();
    s.frame = g_frameIndex.load(std::memory_order_relaxed);
    const UINT subresources = SubresourceCountFor(g_device.Get(), s.desc);
    s.footprints.resize(subresources);
    g_device->GetCopyableFootprints(&s.desc, 0, subresources, 0, s.footprints.data(), nullptr, nullptr, &s.bytes);
    if (!CreateReadbackBuffer(g_device.Get(), s.bytes, s.readback))
    {
        Log("capture: snapshot readback creation failed - skipping one candidate");
        return;
    }
    RecordCopyToReadback(
        cmdList, resource, s.readback.Get(), s.footprints, static_cast<D3D12_RESOURCE_STATES>(stateAfter),
        g_originalResourceBarrier);
    Log(std::string("capture: snapshot recorded - ") + surfaceName + " candidate " +
        std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + DescToString(s.desc) + " on frame " +
        std::to_string(s.frame) + " (" + std::to_string(g_snapshotTaken.size()) + "/" + std::to_string(kMaxSnapshots) +
        ")");
    g_snapshots.push_back(std::move(s));
}

void OnPresent(IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr || g_originalExecuteCommandLists == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    ProcessSnapshots(); // diagnostic; nothing to do unless MV_SNAPSHOT recorded copies

    // An armed burst begins here, at a frame boundary, so the first captured
    // frame is one whose recording was watched from the start.
    if (g_burstArmed.exchange(false, std::memory_order_relaxed))
    {
        g_framesRemaining.store(kCaptureFrames);
        g_burstPresentsLeft.store(kBurstPresentBudget);
        g_burstEndReason.store(BurstEndReason::NotEnded, std::memory_order_relaxed);
        g_manifestDirty.store(true, std::memory_order_relaxed);
        // Frozen once, here (see g_depthRequiredForBurst).
        g_depthRequiredForBurst.store(
            DepthCaptureEnabled() && GetSurfaceRegistry().Selected("depth") != nullptr, std::memory_order_relaxed);
        if (g_dumpStartedUtc.empty())
        {
            g_dumpStartedUtc = UtcNowIso8601();
        }
    }

    // End the burst if the game has stopped rendering the velocity pass -
    // otherwise a run that ends right after F8 spends its whole Present
    // budget silently waiting for frames that will never come.
    {
        const unsigned long long presented = g_frameIndex.load(std::memory_order_relaxed);
        uint64_t silentFor = 0;
        SurfaceIdentifier* velocity = GetSurfaceRegistry().Find("velocity");
        if (g_framesRemaining.load(std::memory_order_relaxed) > 0 && velocity != nullptr &&
            velocity->IsSilent(presented, &silentFor))
        {
            const int short_by = EndBurst(BurstEndReason::VelocityPassSilent);
            Log("capture: burst ENDED - the game has not rendered the velocity pass for " + std::to_string(silentFor) +
                " frames, so there is nothing left to capture. " + std::to_string(short_by) + " of the requested " +
                std::to_string(kCaptureFrames) +
                " never landed and will not; press F8 again once you are back in gameplay. This is "
                "the game's state, not a fault in the capture path - see the 'velocity pass has "
                "STOPPED' line above.");
        }
    }

    // Spend one of the burst's Presents, whatever this frame turns out to
    // produce, and end the burst if it has run out of them.
    if (g_framesRemaining.load(std::memory_order_relaxed) > 0 &&
        g_burstPresentsLeft.fetch_sub(1, std::memory_order_relaxed) <= 1)
    {
        const int short_by = EndBurst(BurstEndReason::PresentBudgetExhausted);
        Log("capture: burst ENDED short - " + std::to_string(kBurstPresentBudget) + " frames presented and " +
            std::to_string(short_by) + " of the requested " + std::to_string(kCaptureFrames) +
            " never landed. The dump has what did. Ending rather than waiting: a burst that "
            "cannot complete also holds the F8 hotkey down for the rest of the session. Check the "
            "'no free slot' and 'WRITE QUEUE FULL' counts above for which limit was hit.");
    }

    // 1. Drain any slots whose GPU work has completed.
    if (g_fence != nullptr)
    {
        const UINT64 completed = g_fence->GetCompletedValue();
        for (Slot& slot : g_slots)
        {
            if (slot.state == SlotState::Submitted && completed >= slot.fenceValue)
            {
                DrainSlot(slot);
                slot.state = SlotState::Free;
            }
        }
    }

    // 1b. Seal the dump directory once nothing from this (or an earlier,
    // still-draining) burst is in flight - which the drain above can make true.
    if (g_manifestDirty.load(std::memory_order_relaxed) && DumpFullyDrained())
    {
        static const char* const kReasonNames[] = {
            "not_ended", "completed", "layout_changed", "velocity_pass_silent", "present_budget_exhausted",
            "surface_reopened"};
        const BurstEndReason reason = g_burstEndReason.load(std::memory_order_relaxed);
        Log("capture: dump directory sealed - " + std::to_string(g_drainedFrames.load(std::memory_order_relaxed)) +
            " frame(s) drained, ended_because=" + kReasonNames[static_cast<int>(reason)] +
            " - writing manifest.json");
        WriteManifestNow();
        g_manifestDirty.store(false, std::memory_order_relaxed);
    }

    // 2. Reclaim slots whose Present never arrived (abandoned frame,
    // resolution change, failed QueryInterface below). Four stranded slots
    // and capture stops silently and permanently.
    const unsigned long long frame = g_frameIndex.load(std::memory_order_relaxed);
    for (Slot& candidate : g_slots)
    {
        if (candidate.state == SlotState::Recording && candidate.frameIndex + kSlotStaleAfterFrames < frame)
        {
            Log("capture: reclaiming stale slot from frame " + std::to_string(candidate.frameIndex) +
                " (never paired with a Present; now at frame " + std::to_string(frame) + ")");
            candidate.state = SlotState::Free;
        }
    }

    // 3. Pair this frame's velocity copy with the finished back buffer.
    Slot* slot = nullptr;
    for (Slot& candidate : g_slots)
    {
        if (candidate.state == SlotState::Recording && candidate.frameIndex == frame)
        {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr || g_device == nullptr || g_queue == nullptr)
    {
        return;
    }
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0)
    {
        // The burst ended mid-frame. Submitting would drive the count below zero
        // (F8 then never re-arms) and write a frame after the manifest sealed.
        slot->state = SlotState::Free;
        return;
    }
    if (!slot->surfaces[kSurfaceVelocity].present)
    {
        // Depth (or nothing) arrived but velocity did not. Every downstream
        // tool keys off velocity, so a frame without it isn't a capture -
        // release the slot rather than emit a half-frame.
        const uint64_t n = g_framesWithoutVelocity.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || n % 60 == 0)
        {
            Log("capture: frame " + std::to_string(frame) + " had no velocity copy (" + std::to_string(n) +
                " such frames so far). Releasing the slot; nothing is written for this frame.");
        }
        slot->state = SlotState::Free;
        return;
    }
    // Every captured frame carries every surface this burst requires. Partial
    // frames cluster at burst edges (F8 lands after depth's edges but before
    // velocity's) and are dropped: inconsistent frames make every offline tool
    // special-case them. A title that never identifies depth still captures.
    const char* missing = nullptr;
    if (!slot->surfaces[kSurfaceDepth].present && g_depthRequiredForBurst.load(std::memory_order_relaxed))
    {
        missing = "depth";
    }
    if (missing != nullptr)
    {
        const uint64_t n = g_incompleteFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3)
        {
            Log("capture: frame " + std::to_string(frame) + " has velocity but not " + missing +
                " (a burst usually starts mid-frame). Dropping it so the dump stays homogeneous - " +
                std::to_string(n) + " so far.");
        }
        slot->state = SlotState::Free;
        return;
    }

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
    {
        return;
    }
    ComPtr<ID3D12Resource> backBuffer;
    const UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
    // GetBuffer AddRefs (unlike view-creation calls), so holding this pointer
    // is safe.
    if (FAILED(swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr)
    {
        return;
    }
    // (Known-source exclusion is fed every Present by d3d12_hook.cpp.)

    // Same every-frame revalidation as velocity: a resize or fullscreen
    // transition recreates swapchain buffers at a new size, and stale
    // footprints would corrupt silently.
    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    if (!DumpLayoutAccepts(kSurfaceColor, desc))
    {
        // The slot stays Recording and is reclaimed by the stale-slot sweep
        // above on a later Present. Nothing is written for this frame, which is
        // the point - it would be the first frame at the new layout.
        return;
    }
    SurfaceCapture& color = slot->surfaces[kSurfaceColor];
    const bool firstColorUse = color.readback == nullptr;
    if (!EnsureReadback(color, kSurfaceColor, desc))
    {
        return;
    }
    if (firstColorUse)
    {
        SnapshotDumpLayout(kSurfaceColor, color);
        if (!g_metadataWritten.exchange(true))
        {
            TruncateFramesCsv();
        }
    }

    // The fence below covers the velocity copy only if the game has already
    // submitted the list it was recorded onto - checked, not assumed.
    if (slot->surfaces[kSurfaceVelocity].coverage == Coverage::Strong)
    {
        g_coverageStrong.fetch_add(1, std::memory_order_relaxed);
    }
    else if (
        g_queueSubmissions.load(std::memory_order_relaxed) >
        slot->surfaces[kSurfaceVelocity].queueSubmissionsAtRecord)
    {
        g_coverageWeak.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        g_coverageNone.fetch_add(1, std::memory_order_relaxed);
        Log("capture: WARNING frame " + std::to_string(frame) +
            " - nothing was submitted to the queue we fence on between recording the velocity copy "
            "and this Present, so the fence below cannot cover it. This frame's velocity readback "
            "is unsafe.");
    }

    if (slot->allocator == nullptr)
    {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot->allocator))))
        {
            return;
        }
        if (FAILED(g_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot->allocator.Get(), nullptr, IID_PPV_ARGS(&slot->commandList))))
        {
            return;
        }
        slot->commandList->Close();
    }

    if (FAILED(slot->allocator->Reset()) || FAILED(slot->commandList->Reset(slot->allocator.Get(), nullptr)))
    {
        return;
    }

    // The back buffer is in PRESENT state here (== COMMON), since the game has
    // finished with it and is about to present.
    RecordCopyToReadback(
        slot->commandList.Get(),
        backBuffer.Get(),
        color.readback.Get(),
        color.footprints,
        D3D12_RESOURCE_STATE_PRESENT,
        g_originalResourceBarrier);
    // Our list, our submit, immediately before the Signal below: nothing to check.
    color.coverage = Coverage::ByConstruction;
    g_coverageByConstruction.fetch_add(1, std::memory_order_relaxed);
    ++color.copies;
    color.present = true;

    if (FAILED(slot->commandList->Close()))
    {
        return;
    }

    ID3D12CommandList* lists[] = {slot->commandList.Get()};
    g_originalExecuteCommandLists(g_queue.Get(), 1, lists);

    slot->fenceValue = g_nextFenceValue++;
    if (FAILED(g_queue->Signal(g_fence.Get(), slot->fenceValue)))
    {
        return;
    }

    slot->captureIndex = g_captureCounter.fetch_add(1);
    slot->state = SlotState::Submitted;

    const int remaining = g_framesRemaining.fetch_sub(1) - 1;
    if (remaining <= 0)
    {
        // No-op if an early-end site already recorded a reason.
        BurstEndReason expectedReason = BurstEndReason::NotEnded;
        g_burstEndReason.compare_exchange_strong(
            expectedReason, BurstEndReason::Completed, std::memory_order_relaxed);
        // "Recorded" is the honest word: last frames may still be in flight
        // on the GPU, and drained frames are only queued for the writer, not
        // necessarily on disk yet. Only ShutdownCapture guarantees that.
        Log("capture: burst recorded, " + std::to_string(g_captureCounter.load()) + " frames; writes still draining (" +
            std::to_string(g_droppedWrites.load()) + " dropped)");
        Log("capture: fence coverage - " + std::to_string(g_coverageStrong.load()) +
            " frames verified by command-list identity, " + std::to_string(g_coverageWeak.load()) +
            " consistent-but-unproven (identity not observable), " + std::to_string(g_coverageNone.load()) +
            " UNSAFE. Only the first category is proof the readback is not stale.");
    }
}

void StopCaptureHotkeyThread(bool processExiting)
{
    g_hotkeyStop.store(true, std::memory_order_relaxed);
    if (!g_hotkeyThread.joinable())
    {
        return;
    }
    if (processExiting)
    {
        // Detach: by DLL_PROCESS_DETACH Windows has already terminated every
        // other thread, and destroying a joinable std::thread calls terminate().
        g_hotkeyThread.detach();
    }
    else
    {
        // The loop's longest wait is a 30ms Sleep, so this returns promptly,
        // and neither GetAsyncKeyState nor Sleep needs the loader lock that may
        // be held while this runs.
        g_hotkeyThread.join();
    }
}

void ShutdownCapture(bool processExiting)
{
    // A live flush (MvFlushCapture) drains the queue and joins. At process exit
    // the writer is already gone. Either way, the writer's "write queue drained"
    // line - not "burst recorded" - is what says it is safe to quit.
    StopCaptureHotkeyThread(processExiting);

    std::thread writer;
    {
        std::lock_guard<std::mutex> lock(g_writeMutex);
        if (!g_writerStarted)
        {
            return;
        }
        g_writerStop = true;
        if (processExiting)
        {
            const size_t pending = g_writeQueue.size();
            if (pending > 0)
            {
                Log("capture: process exiting with " + std::to_string(pending) +
                    " writes still queued - those frames are LOST. Wait for "
                    "'write queue drained' before quitting.");
            }
            if (g_writerThread.joinable())
            {
                g_writerThread.detach(); // same argument as the hotkey thread above
            }
            return;
        }
        writer = std::move(g_writerThread);
    }
    g_writeCv.notify_all();
    if (writer.joinable())
    {
        writer.join();
    }
    Log("capture: writer drained and stopped");
}

} // namespace mv
