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

// Frames per F8 burst: about 20 s at the ~120 fps measured on Skyrunner at
// 960x540.
constexpr int kCaptureFrames = 2400;

// Slots in flight. The GPU trails the CPU by a few frames, so a slot is mapped
// only once its fence has passed.
constexpr int kSlots = 4;

// A Recording slot waits for the Present with its frame index. If that Present
// never comes (abandoned frame, resolution change), the slot is reclaimed.
constexpr unsigned long long kSlotStaleAfterFrames = 8;

// Cap on depth copies per frame. UE5 normally takes depth out of `DEPTH_WRITE`
// once or twice a frame; the cap bounds bandwidth on a title that does more.
constexpr int kMaxDepthCopiesPerFrame = 4;

// Recording: from the frame's first copy until its Present submits it.
enum class SlotState
{
    Free,
    Recording,
    Submitted
};

// Per surface, in `kSurfaceNames` order. `trigger`, `arbitration` and the file
// pattern go into `manifest.json` as written.
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
     "first would truncate it)"},
    {"color", "present_back_buffer", "first_in_frame", ""},
    {"normals", "render_target_to_shader_resource", "first_in_frame", ""},
    {"specular", "render_target_to_shader_resource", "first_in_frame", ""},
};

// How well the fence covers a surface's copy.
//   Strong         The recorded command list was seen submitted to the fenced
//                  queue.
//   Weak           List identity was not observable (the debug layer wraps
//                  lists), but the queue submitted after the copy was recorded.
//   None           Nothing was submitted in between: the readback is unsafe.
//   ByConstruction Our own list, submitted right before the Signal (colour).
//                  Nothing to check, which is a different claim from Strong.
enum class Coverage : uint8_t
{
    None,
    Weak,
    Strong,
    ByConstruction
};

struct SurfaceCapture
{
    ComPtr<ID3D12Resource> readback;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    // The desc the footprints were derived from; footprints are valid for that
    // exact layout only.
    D3D12_RESOURCE_DESC desc{};
    UINT64 bytes = 0;
    UINT subresources = 0;
    int copies = 0;
    bool present = false;
    // The command list this copy was recorded on, as a key for `NoteSubmission`.
    // Never dereferenced. Only velocity's is graded.
    IUnknown* recordedList = nullptr;
    // Submissions on our queue when the copy was recorded: the fallback check
    // when list identity is not observable.
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

// `QueryInterface(IID_IUnknown)` is the only pointer COM guarantees equal for
// one object. Under the debug layer, the command list seen by `ResourceBarrier`
// and the one passed to `ExecuteCommandLists` are different wrappers.
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
// `ComIdentity` of the last list that moved a back buffer to `PRESENT`.
std::atomic<IUnknown*> g_presentingList{nullptr};
ComPtr<ID3D12Fence> g_fence;
UINT64 g_nextFenceValue = 1;

// Fence coverage (see `Coverage`), summarised once per burst.
std::atomic<uint64_t> g_coverageStrong{0};
std::atomic<uint64_t> g_coverageWeak{0};
std::atomic<uint64_t> g_coverageNone{0};
// Colour needs no fence check, so it has its own count.
std::atomic<uint64_t> g_coverageByConstruction{0};
std::atomic<uint64_t> g_queueSubmissions{0};

std::atomic<int> g_framesRemaining{0};

// Most Presents a burst may span. Without it, a burst that never reaches its
// frame count would run forever and keep F8 from arming another.
constexpr int kBurstPresentBudget = kCaptureFrames * 8;
std::atomic<int> g_burstPresentsLeft{0};

// Set by F8; the burst starts at the next Present, so its first frame is
// recorded from the start.
std::atomic<bool> g_burstArmed{false};

// Whether depth is required for this burst, frozen when it starts. Read live,
// a depth reopen mid-burst would stop requiring depth and the dump would mix
// frames with and without it. Frozen, the burst ends instead
// (`NotifySurfaceReopened`).
std::atomic<bool> g_depthRequiredForBurst{false};

void ArmBurst()
{
    g_burstArmed.store(true, std::memory_order_relaxed);
}

// Why `g_framesRemaining` reached zero. Set by compare_exchange against
// `NotEnded`, so the first condition to fire is the one recorded.
enum class BurstEndReason : uint8_t
{
    NotEnded,
    Completed,                // reached the requested frame count
    LayoutChanged,             // a surface's layout changed mid-dump
    VelocityPassSilent,        // the velocity pass stopped rendering
    PresentBudgetExhausted,    // kBurstPresentBudget Presents spent
    // A surface this burst requires reopened. Later frames would come from a
    // different resource, or none.
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

// True when nothing from the current or last burst is in flight: no frames
// left to record and no slot waiting on a fence. Then the dump can be sealed.
// Caller must hold `g_mutex`.
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

// Set when a burst starts, cleared once its manifest is enqueued. Bursts into
// one `MV_DUMP_DIR` share one manifest, which describes the whole directory.
std::atomic<bool> g_manifestDirty{false};

// Frames skipped because every ring slot was in flight. Tells a busy ring
// from a wedged one.
std::atomic<uint64_t> g_noFreeSlot{0};
std::atomic<unsigned long long> g_frameIndex{0};
std::atomic<int> g_captureCounter{0};
std::atomic<bool> g_metadataWritten{false};

// The F8 thread is joinable, with a stop flag. A detached thread crashes when
// `FreeLibrary` unmaps the module under it: a game never unloads the hook, but
// the testhost does on every run.
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
// `manifest.json` has one layout per surface (`layouts[0]`). After a resolution
// change, offline tools would un-pad later frames at the old row pitch and get
// a wrong image without an error, so a layout change ends the burst.
struct DumpLayout
{
    D3D12_RESOURCE_DESC desc{};
    bool known = false;
    // Recorded the first time the surface is set up for this dump.
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

// Caller must hold `g_mutex`. Records the layout the first time a surface is
// seen; afterwards returns false, having ended the burst, if it changed. Not
// reset per burst: bursts into one directory share a manifest.
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
        DescToString(desc) + ". A dump holds one layout; " + std::to_string(short_by) +
        " requested frames not captured. Press F8 again with a new MV_DUMP_DIR.");
    return false;
}

// ---------------------------------------------------------------------------
// Background writer. Disk I/O stays off the render thread: a synchronous write
// per frame is a stall, and stalls have exposed bugs in games.
// The render thread only copies the bytes out and hands them over.
// ---------------------------------------------------------------------------

struct WriteJob
{
    std::string path;
    std::vector<uint8_t> data;
    bool append = false;
    // Manifest only: `path` is a .tmp file, renamed over this after a full
    // write. `MoveFileExA` replaces atomically, so a reader never sees half.
    std::string renameTo;
};

// Backpressure cap. Past it, frames are dropped and counted: an unbounded queue
// risks running the game out of memory when the disk falls behind.
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
// Frames that reached the write queue (`g_captureCounter` counts slots
// submitted to the GPU). Reported as `burst.drained_frames`.
std::atomic<uint64_t> g_drainedFrames{0};

// Resolved and created once; it is called per blob on the render thread.
// `MV_DUMP_DIR` keeps testhost runs out of real captures.
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

// `MV_CAPTURE_DEPTH=0` turns the depth copy off for the session, to measure
// its bandwidth cost. Read once: it is checked on the hot path, and changing
// it mid-session would make frames unlike each other.
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
            Log("capture: MV_CAPTURE_DEPTH=0 - depth is not captured this session.");
        }
        return !off;
    }();
    return enabled;
}

// Writes one job. False, logged and counted, if the file could not be opened or
// fully written. A manifest is renamed into place only after a full write.
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
            ") err=" + std::to_string(GetLastError()) + ". The frames are on disk but have no manifest.");
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

        // This line, not "burst recorded", is the signal that it is safe to
        // quit.
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

// Room for `bytes` more? Checked before mapping, so a frame is dropped whole
// instead of losing one surface.
bool WriteQueueHasRoom(size_t bytes)
{
    std::lock_guard<std::mutex> lock(g_writeMutex);
    return g_queuedBytes + bytes <= kMaxQueuedBytes;
}

// False if the job was dropped for backpressure. A manifest write (`renameTo`
// set) always goes through.
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
        // frames.csv and the manifest always go through: without them the
        // frames on disk cannot be read.
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

// Enqueues one frame's blobs and its frames.csv line under one hold of
// `g_writeMutex`: all or none. False if dropped for backpressure, which
// `DrainSlot`'s room check should already have prevented.
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

// Plane count of a format: 2 for depth/stencil, 1 for most. Subresources are
// mips * array size * planes; a wrong count truncates the copy.
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

// Makes sure a surface has a readback buffer matching `desc`, rebuilding the
// footprints when the layout changed. Checked every frame because resolution
// can change mid-session, and stale footprints corrupt the copy. False if the
// buffer could not be created. Caller must hold `g_mutex`.
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

// Copies every subresource of `source` into a READBACK buffer, between
// barriers that restore its state. Subresources go back to back in the order
// `GetCopyableFootprints` gives, so depth's stencil plane and any mips land in
// the same file.
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

    // `ALL_SUBRESOURCES` covers every plane in one barrier.
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

    // Restore the game's state, or its own barrier tracking goes out of step.
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = stateBefore;
    barrierFn(cmdList, 1, &barrier);
}

// `MV_ENGINE_VERSION=<major>.<minor>`, for the manifest's "engine" block. Never
// guessed: 5.7 packs velocity channel 3 differently from 5.2.
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

// Truncates frames.csv to its header, so a new session never appends to an
// old one.
void TruncateFramesCsv()
{
    const std::string header = "# captureIndex,gameFrameIndex\n";
    EnqueueWrite(DumpDir() + "frames.csv", std::vector<uint8_t>(header.begin(), header.end()));
}

// ---------------------------------------------------------------------------
// manifest.json. Depth's layout lists
// both planes: D24S8 and D32S8 keep depth in plane 0 and stencil in plane 1,
// with different formats and row pitches.

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

// One plane's footprint as a manifest object.
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

// Builds `manifest.json`. Caller must hold `g_mutex`.
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
    // "embedded" means the DLL's own `profiles/ue5.json`, pinned by
    // `producer.commit`; anything else is an `MV_PROFILE_FILE` path.
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
            // Tells "off" from "on but never found".
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
            // No optional surface's encoding is verified; "unknown" makes
            // `mvtools.py` refuse to decode it.
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
            // Rides the game's list like velocity's, but its coverage is
            // not graded.
            j += ",\n      \"coverage_note\": \"fence coverage is not evaluated for this surface\"\n";
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
    j += "    \"columns\": [\"capture_index\", \"game_frame_index\"]\n";
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

// Enqueues the manifest after every blob it names. The writer is FIFO, so the
// blobs are on disk before the manifest appears.
void WriteManifestNow()
{
    const std::string json = BuildManifest();
    EnqueueWrite(
        DumpDir() + "manifest.json.tmp",
        std::vector<uint8_t>(json.begin(), json.end()),
        /*append=*/false,
        /*renameTo=*/DumpDir() + "manifest.json");
}

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

// Maps a finished slot's readback buffers and hands the bytes to the writer:
// the whole frame or none of it.
bool DrainSlot(Slot& slot)
{
    // Room is checked before any `Map()`, from the footprint sizes, so a
    // dropped frame costs no copy out of GPU memory.
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
        if (n <= 3 || n % 100 == 0)
        {
            Log("capture: write queue full, dropping frame " + std::to_string(slot.captureIndex) + " (" +
                std::to_string(total / (1024 * 1024)) + " MB) - " + std::to_string(n) + " dropped so far");
        }
        return false;
    }

    // Every tool keys off velocity and the back buffer; a slot without both is
    // not a capture.
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

    // The game frame this capture came from. Capture indices only count what
    // was drained; warp validation needs consecutive frames.
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

void NotePresentingList(ID3D12GraphicsCommandList* cmdList)
{
    // D3D12 `PRESENT` and `COMMON` are the same state, so the barrier hook also
    // calls this for a copy or compute list that returns a back buffer to
    // `COMMON`. Only a direct list can present.
    if (g_queue == nullptr && cmdList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        g_presentingList.store(ComIdentity(cmdList), std::memory_order_relaxed);
    }
}

void NoteCommandQueue(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists)
{
    IUnknown* const presentingList = g_presentingList.load(std::memory_order_relaxed);
    if (queue == nullptr || commandLists == nullptr || presentingList == nullptr || g_queue != nullptr)
    {
        return;
    }

    bool presents = false;
    for (UINT i = 0; i < numCommandLists && !presents; ++i)
    {
        presents = ComIdentity(commandLists[i]) == presentingList;
    }

    if (!presents)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue == nullptr)
    {
        g_queue = queue;
        Log("capture: noted the presenting direct command queue");
    }
}

ID3D12CommandQueue* GetPresentingQueue()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_queue.Get();
}

std::string CaptureStatusLine()
{
    const int remaining = g_framesRemaining.load(std::memory_order_relaxed);
    if (remaining > 0)
    {
        return "capture: recording, " + std::to_string(kCaptureFrames - remaining) + " of " +
               std::to_string(kCaptureFrames) + " frames";
    }

    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(g_writeMutex);
        queued = g_writeQueue.size();
    }

    return queued > 0 ? "capture: writing, " + std::to_string(queued) + " files queued. Wait before quitting."
                      : "capture: idle";
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
            // `MV_AUTOCAPTURE`: for mv_testhost, which cannot press F8.
            if (GetEnvironmentVariableA("MV_AUTOCAPTURE", nullptr, 0) != 0)
            {
                Sleep(1000);
                ArmBurst();
                Log("capture: MV_AUTOCAPTURE set, starting a burst without a keypress");
            }
            bool wasDown = false;
            bool restartWasDown = false;
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

                const bool restartIsDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
                if (restartIsDown && !restartWasDown)
                {
                    GetSurfaceRegistry().RequestRestart();
                }
                restartWasDown = restartIsDown;

                Sleep(30);
            }
        });
    Log("capture: hotkey thread started (F8 = capture burst, F6 = restart identification)");
}

namespace
{

// Caller must hold `g_mutex`.
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

// The slot this frame's copies go into: the one already recording this frame,
// or a free one. Caller must hold `g_mutex`.
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
            // The readback buffers are kept for reuse; `EnsureReadback`
            // revalidates the layout before each copy.
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

// Every slot is in flight: the frame is skipped, not waited for. Logged, or a
// wedged ring would look the same. Caller must hold `g_mutex`.
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

    // Which of a frame's edges lands (the `arbitration` in `kCaptureFacts`).
    if (surface == kSurfaceDepth)
    {
        // last_before_surface: depth goes readable several times a frame, and
        // the first edge can be a partial pass (on Skyrunner, the weapon only),
        // so it is copied again on each edge until velocity has been copied. A
        // title whose depth goes readable only after velocity still gets its
        // first edge.
        const bool velocityDone = slot->surfaces[kSurfaceVelocity].present;
        if (capture.copies >= kMaxDepthCopiesPerFrame || (velocityDone && capture.copies > 0))
        {
            return;
        }
        if (velocityDone && !g_depthAfterVelocityReported.exchange(true))
        {
            Log("capture: depth becomes readable only after velocity on this title, so the dump's depth "
                "comes from a pass after the velocity pass.");
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

    // Recorded on the game's list. `NoteSubmission` checks that the fence
    // covers it.
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
    // Runs for every submission; nothing to check outside a burst.
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
        // Velocity only: the depth and GBuffer copies also ride the game's
        // list but are not graded.
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
                // Capture signals its fence on the presenting queue, so the
                // fence covers only work submitted there. A velocity list on
                // another queue can still be running when the fence passes,
                // which makes the readback stale.
                Log("capture: WARNING velocity command list submitted on another queue - frame " +
                    std::to_string(slot.frameIndex) + " is not covered by our fence");
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
    // Velocity is always required; depth as frozen for this burst
    // (`g_depthRequiredForBurst`).
    const bool required = name == "velocity" || (name == "depth" && g_depthRequiredForBurst.load(std::memory_order_relaxed));
    if (!required)
    {
        return;
    }
    const int short_by = EndBurst(BurstEndReason::SurfaceReopened);
    Log("capture: burst ENDED - the " + name + " search reopened mid-burst. " + std::to_string(short_by) +
        " of " + std::to_string(kCaptureFrames) + " requested frames not captured. Press F8 again once it has "
        "settled.");
}

// ---------------------------------------------------------------------------
// `MV_SNAPSHOT=<surface>`: copies one frame of each resource that surface has
// shortlisted at the scene extent, at its own RT -> SRV edge, to
// `snapshot_<surface>_<ptr>_<w>x<h>_fmt<f>_pitch<p>.bin` in the dump directory.
// For telling candidates apart by their pixels when every barrier signal ties
// them; this is how GBufferB was found among seven B8G8R8A8_TYPELESS targets.
// Changes no selection. The copy rides the game's list, and the fence is
// signalled at a later Present, after that list was submitted. Guarded by
// `g_mutex`.
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

// Caller must hold `g_mutex`.
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

    // A burst starts at a frame boundary, so its first frame is recorded from
    // the start.
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

    // End the burst if the game stopped rendering the velocity pass (a menu),
    // instead of waiting out the whole Present budget.
    {
        const unsigned long long presented = g_frameIndex.load(std::memory_order_relaxed);
        uint64_t silentFor = 0;
        SurfaceIdentifier* velocity = GetSurfaceRegistry().Find("velocity");
        if (g_framesRemaining.load(std::memory_order_relaxed) > 0 && velocity != nullptr &&
            velocity->IsSilent(presented, &silentFor))
        {
            const int short_by = EndBurst(BurstEndReason::VelocityPassSilent);
            Log("capture: burst ENDED - no velocity pass for " + std::to_string(silentFor) + " frames (a menu?). " +
                std::to_string(short_by) + " of " + std::to_string(kCaptureFrames) +
                " requested frames not captured. Press F8 again in gameplay.");
        }
    }

    // Every Present counts against the burst's budget.
    if (g_framesRemaining.load(std::memory_order_relaxed) > 0 &&
        g_burstPresentsLeft.fetch_sub(1, std::memory_order_relaxed) <= 1)
    {
        const int short_by = EndBurst(BurstEndReason::PresentBudgetExhausted);
        Log("capture: burst ENDED - " + std::to_string(kBurstPresentBudget) + " frames presented, " +
            std::to_string(short_by) + " of " + std::to_string(kCaptureFrames) +
            " requested frames not captured. See the 'no free slot' and 'write queue full' lines.");
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

    // 1b. Seal the dump directory once nothing from this or an earlier burst
    // is in flight, which the drain above can make true.
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

    // 2. Reclaim slots whose Present never came (abandoned frame, resolution
    // change, failed `QueryInterface` below). Four stranded slots would stop
    // capture for good.
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
        // Depth arrived but velocity did not. Every tool keys off velocity, so
        // nothing is written for this frame.
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
    // frames happen at burst edges (F8 lands after depth's edges but before
    // velocity's) and are dropped. A title that never identifies depth still
    // captures.
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
            Log("capture: frame " + std::to_string(frame) + " has velocity but no " + missing +
                " (a burst starting mid-frame); not written. " + std::to_string(n) + " so far.");
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
    // `GetBuffer` adds a reference, so holding the pointer is safe.
    if (FAILED(swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr)
    {
        return;
    }

    // Revalidated every frame like the others: a resize or fullscreen switch
    // recreates the swapchain buffers.
    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    if (!DumpLayoutAccepts(kSurfaceColor, desc))
    {
        // The slot stays Recording and is reclaimed by the stale-slot sweep.
        // This would have been the first frame at the new layout.
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
    // submitted the list it is on (see `NoteSubmission`).
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
            " - nothing was submitted to the fenced queue since the velocity copy; its readback is unsafe.");
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

    // The game is done with the back buffer and about to present: it is in
    // `PRESENT` (== `COMMON`).
    RecordCopyToReadback(
        slot->commandList.Get(),
        backBuffer.Get(),
        color.readback.Get(),
        color.footprints,
        D3D12_RESOURCE_STATE_PRESENT,
        g_originalResourceBarrier);
    // Our list, submitted right before the Signal below: nothing to check.
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
        // No-op if an early end already recorded a reason.
        BurstEndReason expectedReason = BurstEndReason::NotEnded;
        g_burstEndReason.compare_exchange_strong(
            expectedReason, BurstEndReason::Completed, std::memory_order_relaxed);
        // "Recorded": the last frames may still be on the GPU or in the write
        // queue.
        Log("capture: burst recorded, " + std::to_string(g_captureCounter.load()) + " frames; writes still draining (" +
            std::to_string(g_droppedWrites.load()) + " dropped)");
        Log("capture: fence coverage - " + std::to_string(g_coverageStrong.load()) + " verified, " +
            std::to_string(g_coverageWeak.load()) + " probable (list identity not observable), " +
            std::to_string(g_coverageNone.load()) + " UNSAFE");
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
        // Detach: at `DLL_PROCESS_DETACH` Windows has already ended every
        // other thread, and destroying a joinable `std::thread` terminates.
        g_hotkeyThread.detach();
    }
    else
    {
        // Returns within one 30 ms `Sleep`, and nothing in the loop needs the
        // loader lock that may be held here.
        g_hotkeyThread.join();
    }
}

void ShutdownCapture(bool processExiting)
{
    // A live flush (`MvFlushCapture`) drains the queue and joins. At process
    // exit the writer is already gone. Either way, the writer's "write queue
    // drained" line is the signal that it is safe to quit.
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
                    " writes queued - those frames are LOST. Wait for 'write queue drained' before quitting.");
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
