#include "capture.h"

#include "build_info.h"
#include "logging.h"
#include "surface_profile.h"
#include "surface_registry.h"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cassert>
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

// Frames per F8 burst; each frame writes velocity + back buffer. Tuned for
// ~20s of real gameplay at this title's observed ~120fps at reduced
// resolution (960x540) - measured 116-123fps across two bursts, not a fixed
// engine rate, so this is an estimate rather than an exact figure.
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

// ---------------------------------------------------------------------------
// Surfaces.
//
// A Surface is one per-frame image the dump carries. Deliberately not called a
// "plane", "resource" or "buffer": all three already mean something else here
// (ID3D12Resource, D3D12_RESOURCE_DIMENSION_BUFFER, and D3D12's own
// depth/stencil subresource PLANES, which live inside a Surface and keep that
// word).
//
// This replaces five field sets that were triplicated across Slot by hand -
// buffer, footprints, desc, bytes, subresources - plus six more that existed
// for one surface each (four of them velocity's, one depth's, none colour's).
// Adding a fourth surface meant editing eleven places, and a forgotten one
// degraded silently. See docs/REFACTOR_PLAN.md.
//
// A fixed array rather than the std::vector the plan sketched: the count is
// known at compile time until profiles arrive (Phase 9), and an array keeps
// "no allocation on the hot path" true by construction rather than by
// remembering to reserve.
constexpr int kMaxSurfaces = 8;
constexpr int kSurfaceVelocity = 0;
constexpr int kSurfaceDepth = 1;
constexpr int kSurfaceColor = 2;
// Optional surfaces (docs/REFACTOR_PLAN.md Phase 10): every index from
// kFirstOptionalSurface on. Each is captured only when MV_SURFACES named it
// (registering its identifier), is never required - a frame without it is
// still complete - and shares one capture path (OnOptionalSurfaceReadable).
// Adding one is an entry here, in kSurfaceSpecs/kStaticFacts, and in
// surface_profile.cpp's kOptionalSurfaces - nothing else.
constexpr int kSurfaceNormals = 3;  // GBufferA
constexpr int kSurfaceSpecular = 4; // GBufferB: metallic, specular, roughness, shading model
constexpr int kFirstOptionalSurface = kSurfaceNormals;
constexpr int kSurfaceCount = 5;
static_assert(kSurfaceCount <= kMaxSurfaces, "surface count outgrew the fixed array");

// Whether a surface's identity is FOUND (structural + behavioural search) or
// GIVEN (a documented API contract names the exact resource, so there is
// nothing to search for). Two states, not a spectrum: docs/REFACTOR_PLAN.md
// sec 3.1 is explicit that a surface may claim KnownByConstruction only when
// an API contract names it, and colour is the only surface in D3D12 that
// qualifies - there is no GetVelocityBuffer(), no GetSceneDepth().
//
// This is a closed enum, not a free-form flag, and deliberately carries no
// "assume" or "skip the gate" option: a profile can SELECT
// KnownByConstruction for a surface that already has one (there is exactly
// one candidate today, below), but adding a second is a code change, not a
// config change (sec 2.8 - a profile may narrow/widen/reweight a search, it
// may never assert an identity outright).
enum class SourceKind : uint8_t
{
    Identified,
    KnownByConstruction
};

// The specific API contract, when SourceKind::KnownByConstruction applies.
// Meaningless otherwise - kept as its own enum rather than a bool so a second
// known source (there is none today) does not have to overload this one's
// meaning.
enum class KnownSource : uint8_t
{
    None,
    SwapChainBackBuffer
};

// The data-only half of a surface's description: scalars and names, no state,
// nothing that survives a call. That is what will let it come out of a JSON
// profile later without changing shape.
struct SurfaceSpec
{
    const char* name;       // for logs
    const char* filePrefix; // dump filename stem, as in vel_00001.bin
    // Appended to this surface's readback-creation log line. Per-surface facts
    // worth stating once, kept as data rather than as a branch inside the
    // shared path that writes them.
    const char* note;
    SourceKind source;
    KnownSource known; // meaningful only when source == KnownByConstruction
};

constexpr SurfaceSpec kSurfaceSpecs[kSurfaceCount] = {
    {"velocity", "vel", "", SourceKind::Identified, KnownSource::None},
    {"depth",
     "depth",
     " (plane count comes from the desc: a D24S8 or D32S8 depth buffer is two planes, and copying only the "
     "first would silently truncate it)",
     SourceKind::Identified,
     KnownSource::None},
    {"back buffer", "color", "", SourceKind::KnownByConstruction, KnownSource::SwapChainBackBuffer},
    {"normals", "normals", "", SourceKind::Identified, KnownSource::None},
    {"specular", "specular", "", SourceKind::Identified, KnownSource::None},
};

// How well the fence covers a surface's copy.
//
//   Strong         - the exact recorded command list was seen submitted to the
//                    fenced queue.
//   Weak           - list identity wasn't observable (e.g. under the debug
//                    layer's wrapper), but the queue did submit after
//                    recording. Consistent, not proven.
//   None           - nothing submitted in between; readback is unsafe.
//   ByConstruction - the copy was recorded on OUR list and submitted by US
//                    immediately before the Signal, so there is nothing to
//                    check. This is the back buffer, and it is a distinct
//                    value rather than a Strong that would mean something
//                    materially different.
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
    // Replaces hasVelocity/hasDepth - and gives the back buffer the flag it
    // never had, where presence was implied by MapOut happening to succeed.
    bool present = false;
    // Identity of the command list this copy was recorded onto. Used only as a
    // key to check submission - never dereferenced, we hold no ref. This was a
    // single field on Slot that only the velocity path ever set, which is why
    // depth's fence coverage has never actually been evaluated.
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
    SurfaceCapture surfaces[kMaxSurfaces];
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

// Fence-coverage accounting, summarised once per burst. Three outcomes:
//   strong - the exact recorded command list was seen submitted to the fenced
//            queue; the fence covers the copy.
//   weak   - list identity wasn't observable (e.g. under the debug layer's
//            wrapper), but the queue did submit after recording. Consistent,
//            not proven.
//   none   - nothing submitted in between; readback is genuinely unsafe.
std::atomic<uint64_t> g_coverageStrong{0};
std::atomic<uint64_t> g_coverageWeak{0};
std::atomic<uint64_t> g_coverageNone{0};
// Colour's equivalent of the three above: tallied once per frame, right
// where color.coverage is set to ByConstruction. Not "strong" - a materially
// different fact (no fence check was needed at all, not "one was performed
// and passed") - so it gets its own counter rather than folding into
// g_coverageStrong.
std::atomic<uint64_t> g_coverageByConstruction{0};
std::atomic<uint64_t> g_queueSubmissions{0};

std::atomic<int> g_framesRemaining{0};

// Hard cap on Presents a burst may span. g_framesRemaining alone counts
// frames submitted, so a burst that never lands the requested count would
// otherwise run forever with F8 dead for the rest of the session.
constexpr int kBurstPresentBudget = kCaptureFrames * 8;
std::atomic<int> g_burstPresentsLeft{0};

// Armed by the hotkey; begins at the next Present. F8 can arrive mid-frame,
// after depth edges or View-buffer binds have already gone past - starting
// at the next frame boundary guarantees the first captured frame is seen
// from the start.
std::atomic<bool> g_burstArmed{false};

// Frozen at burst arm (docs/REFACTOR_PLAN.md sec 2.7 mechanism 1, finding
// 1.14): whether depth was identified (and enabled) at the moment THIS burst
// started, not re-evaluated live every frame. The pre-Phase-8 completeness
// check read IdentifiedDepthResource() != nullptr fresh on every Present, so
// a mid-burst depth reopen made the gate stop requiring depth the instant
// identification went live-null - producing a dump that mixes frames with
// and without depth while passing its own homogeneity check. Freezing this
// once, at arm time, makes that impossible: either every frame in the burst
// is held to the same requirement, or (see NotifySurfaceReopened) the burst
// ends instead of drifting.
std::atomic<bool> g_depthRequiredForBurst{false};

// Arms the burst for the next Present.
void ArmBurst()
{
    g_burstArmed.store(true, std::memory_order_relaxed);
}

// The four ways a burst's g_framesRemaining reaches zero. Recorded with
// compare_exchange against NotEnded at each site (see EndBurst below), so
// whichever condition fires first for a given burst-arm cycle is what gets
// remembered - correct regardless of which check happens to run first in
// OnPresent, or whether more than one would fire in the same call.
enum class BurstEndReason : uint8_t
{
    NotEnded,
    Completed,                // reached the requested frame count
    LayoutChanged,             // a surface's layout changed mid-dump
    VelocityPassSilent,        // the velocity pass stopped rendering
    PresentBudgetExhausted,    // kBurstPresentBudget Presents spent
    // A surface required for this burst (frozen at arm time, see
    // g_depthRequiredForBurst) reopened mid-burst - docs/REFACTOR_PLAN.md
    // finding 1.14/test 3. Ending honestly beats the pre-Phase-8 behaviour of
    // silently dropping frames (or, before the frozen-mask fix, writing
    // frames without the reopened surface at all once IdentifiedXResource()
    // went live-null and the completeness check's live read stopped
    // requiring it).
    SurfaceReopened,
};
std::atomic<BurstEndReason> g_burstEndReason{BurstEndReason::NotEnded};

// Zeroes g_framesRemaining and records why, if nothing already has. Returns
// how many requested frames were never captured, same as the raw exchange
// callers used before this existed - kept so every call site's own Log()
// line is unchanged.
int EndBurst(BurstEndReason reason)
{
    BurstEndReason expected = BurstEndReason::NotEnded;
    g_burstEndReason.compare_exchange_strong(expected, reason, std::memory_order_relaxed);
    return g_framesRemaining.exchange(0, std::memory_order_relaxed);
}

// True once nothing that belongs to the current (or most recently ended)
// burst is still in flight: no frames left to record AND no slot waiting on
// a fence. This is the "safe to seal the dump directory" signal - see the
// header comment above for why it is not simply "g_framesRemaining == 0".
// Caller must hold g_mutex (reads Slot::state).
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

// Set whenever a burst is armed; cleared once DumpFullyDrained() has been
// observed true and the manifest write for it has been enqueued. Two
// bursts into the same MV_DUMP_DIR share this flag - frames.csv worked the
// same way even before manifest.json existed (see TruncateFramesCsv) - so a
// second burst re-dirties it, and the eventual seal reflects the cumulative
// state of the whole directory, not just the latest burst.
std::atomic<bool> g_manifestDirty{false};

// Frames skipped because every ring slot was in flight. Diagnostic only -
// distinguishes "ring busy as designed" from "ring wedged".
std::atomic<uint64_t> g_noFreeSlot{0};
std::atomic<unsigned long long> g_frameIndex{0};
std::atomic<int> g_captureCounter{0};
std::atomic<bool> g_metadataWritten{false};

// F8 hotkey thread, joinable rather than detached, with a stop flag.
//
// A detached thread looping forever crashes on the FreeLibrary teardown
// path: the module unmaps while the thread is still running, so its next
// instruction is in freed memory (STATUS_STACK_BUFFER_OVERRUN). Never seen
// in a shipping game, since it never unloads; testhost exercises teardown
// on every run.
std::atomic<bool> g_hotkeyStop{false};
std::thread g_hotkeyThread;

std::atomic<uint64_t> g_depthEdgeReports{0};
std::atomic<bool> g_depthAfterVelocityReported{false};
// Frames where depth arrived but velocity never did. Counted rather than
// silently discarded - worth surfacing in the log.
std::atomic<uint64_t> g_framesWithoutVelocity{0};
std::atomic<uint64_t> g_incompleteFrames{0};

// ---------------------------------------------------------------------------
// One dump, one layout.
//
// manifest.json's "layouts" array (see BuildManifest) has exactly one entry,
// epoch 0 - a genuine schema limitation, not yet the multi-epoch design
// docs/REFACTOR_PLAN.md sec 4.1 sketches. Nothing describes a resource's
// layout as a per-frame fact, only a per-dump one, so a resolution change
// mid-dump has nowhere to be recorded once the first layout has been
// snapshotted (see SnapshotDumpLayout).
//
// Originally this guarded meta.txt/meta_depth.txt, which had the same
// one-shot-write limitation for a stronger reason: they were written ONCE,
// full stop, with no mechanism to update them at all - so a layout change
// after that point left the file describing a layout that no longer
// matched later frames, invisibly. mvtools._rows() would un-pad rows at
// whatever row pitch it was handed and return a plausible image at the
// wrong stride - not an error, a picture. This is the same class of bug as
// decoding velocity as linear: wrong everywhere, and it looks fine.
// manifest.json's single epoch is a milder version of the same limitation:
// at least it is consistent for the frames it does describe, but a dump
// spanning a resolution change still cannot represent both layouts.
//
// Ending the burst is the current, honest answer to that gap. A dump holds
// exactly the layout it was opened at, and a layout change closes it rather
// than silently mis-describing later frames.
struct DumpLayout
{
    D3D12_RESOURCE_DESC desc{};
    bool known = false;
    // Snapshotted once, the first time this surface is established for the
    // dump - see SnapshotDumpLayout. Empty until then.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    UINT64 bytes = 0;
    UINT subresources = 0;
};

DumpLayout g_dumpLayout[kMaxSurfaces];

// Records the derived layout (footprints, byte count, subresource count) the
// first time a surface's readback is established for this dump - a no-op on
// every later call, since DumpLayoutAccepts already refuses any layout that
// would make this stale. Caller must hold g_mutex.
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
// changed. Deliberately NOT reset when a new burst is armed: two bursts into
// one dump directory share one manifest.json (and shared one meta.txt before
// it existed), so the constraint is per dump, not per burst.
bool DumpLayoutAccepts(int surface, const D3D12_RESOURCE_DESC& desc)
{
    const char* name = kSurfaceSpecs[surface].name;
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
// synchronous per-frame write there is a stall that can expose latent bugs
// in the game (see DEBUGGING.md). Render thread only memcpy's out of the
// mapped buffer and hands the bytes off.
// ---------------------------------------------------------------------------

struct WriteJob
{
    std::string path;
    std::vector<uint8_t> data;
    bool append = false;
    // Non-empty only for the manifest: written to `path` (a .tmp file) and,
    // once that succeeds, atomically renamed over this. MoveFileExA is the
    // Windows atomic-rename primitive - a reader sees the old file or the
    // whole new one, never a torn one.
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
// Frames that actually reached the write queue, as opposed to g_captureCounter
// (which counts slots that reached Submitted - a copy recorded on the GPU,
// not yet proven written). Phase 4c's manifest reports this as
// burst.drained_frames.
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
// isolation test, not a feature, to separate depth-copy cost from other
// causes of dropped frames.
//
// Read once: it's consulted on the barrier hot path, and changing mid-session
// would produce frames that aren't alike (see the completeness rule below).
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

// Is there room for `bytes` more of queued output? Checked so a frame can be
// dropped WHOLE rather than per-blob, which produced dumps missing only one
// artefact with nothing recording which frames were affected.
bool WriteQueueHasRoom(size_t bytes)
{
    std::lock_guard<std::mutex> lock(g_writeMutex);
    return g_queuedBytes + bytes <= kMaxQueuedBytes;
}

// Returns false if the job was dropped for backpressure. `renameTo` non-empty
// marks this as a manifest-style write (see WriteJob) and always goes
// through regardless of size, same as frames.csv already does.
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
        // Small bookkeeping writes (metadata, frame index sidecar, manifest)
        // always go through - dropping them loses the ability to interpret
        // the frames that did make it.
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

// Enqueues every blob belonging to one frame - plus its frames.csv line, when
// present - as a single atomic unit: all of them land in the write queue
// together, under one hold of g_writeMutex, or none do.
//
// Before this, each surface's blob went through its own EnqueueWrite call,
// each re-checking the queue budget independently. That could not actually
// drop only part of a frame - DrainSlot has exactly one caller (OnPresent,
// itself called only while g_mutex is held), so nothing else can grow the
// queue between one blob's check and the next - but the code did not SAY
// that; it relied on a reader noticing and preserving an invariant nothing
// enforced. This makes the partial case impossible to express instead of
// merely absent in practice.
//
// Returns false if the whole frame was dropped for backpressure. Given the
// size-before-map check in DrainSlot, this recheck should never actually
// fail - but the return value stays honest rather than assumed.
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
// footprints if the layout changed or this is the slot's first use.
//
// Footprints are only valid for the exact layout they were derived from, so the
// desc is compared every frame rather than trusted once: render resolution can
// change mid-session (dynamic resolution scaling), and stale footprints would
// silently corrupt the copy into plausible-looking garbage.
//
// This was three near-identical blocks, one per surface, differing only in
// which Slot fields they touched and what they logged - which is why adding a
// fourth surface meant transcribing it a fourth time.
//
// Returns false if the buffer could not be created, in which case nothing is
// recorded for that surface this frame. Caller must hold g_mutex.
bool EnsureReadback(SurfaceCapture& capture, int surface, const D3D12_RESOURCE_DESC& desc)
{
    if (capture.readback != nullptr && SameLayout(capture.desc, desc))
    {
        return true;
    }
    const SurfaceSpec& spec = kSurfaceSpecs[surface];
    if (capture.readback != nullptr)
    {
        Log(std::string("capture: ") + spec.name + " layout changed, " + DescToString(capture.desc) + " -> " +
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
        Log(std::string("capture: failed to create ") + spec.name + " readback buffer");
        return false;
    }
    Log(std::string("capture: ") + spec.name + " readback buffer created, " + std::to_string(total) + " bytes, " +
        std::to_string(subresources) +
        " subresource(s), rowPitch=" + std::to_string(capture.footprints[0].Footprint.RowPitch) + spec.note);
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

// The engine version the dump was captured against, if the operator supplied
// one. Used by BuildManifest's "engine" block - a leftover from when this
// also fed meta.txt, kept as its own function rather than inlined so there
// is still exactly one parser if a second consumer ever needs it.
//
// Operator-supplied via MV_ENGINE_VERSION=5.2; unset stays unset rather than
// guessing - matters because 5.7.1 steals bits of channel 3 for
// bHasPixelAnimation and 5.2 doesn't, so a wrong guess mis-decodes silently.
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

// meta.txt is gone (docs/REFACTOR_PLAN.md Phase 5 - manifest.json's
// "layouts" block supersedes it; see BuildManifest). This keeps the other
// half of what WriteMetadata used to do: truncating frames.csv's header so
// a new session never appends onto a previous one's capture indices.
void TruncateFramesCsv()
{
    const std::string header = "# captureIndex,gameFrameIndex\n";
    EnqueueWrite(DumpDir() + "frames.csv", std::vector<uint8_t>(header.begin(), header.end()));
}

// ---------------------------------------------------------------------------
// manifest.json - see docs/REFACTOR_PLAN.md sec 4.1 for the target schema
// and this file's header comment for what is deliberately not yet in it.
//
// Depth's layout (including both planes - D24S8/D32S8 are MULTI-PLANE, depth
// in plane 0 and stencil in plane 1, with different formats and row
// pitches) lives in BuildManifest's "layouts" block, sourced from
// g_dumpLayout via SnapshotDumpLayout. A dump with depth "not present" in
// the manifest is a dump without depth - exactly what it looks like.

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

// Static per-surface facts the manifest reports (trigger edge, arbitration
// rule, file pattern). Written as literals here rather than as SurfaceSpec
// fields: nothing in the code is DATA-DRIVEN by them yet (OnVelocityReadable
// and OnDepthReadable are hardcoded functions, not dispatched off a spec) -
// adding fields nothing reads would be exactly the kind of half-finished
// abstraction the migration plan warns against. They become real SurfaceSpec
// fields when SurfaceIdentifier (Phase 6-7) is what actually reads them.
struct StaticSurfaceFacts
{
    const char* trigger;
    const char* arbitration;
    const char* filePattern;
};
constexpr StaticSurfaceFacts kStaticFacts[kSurfaceCount] = {
    {"render_target_to_shader_resource", "first_in_frame", "vel_%05d.bin"},
    {"depth_write_to_readable", "last_before_surface", "depth_%05d.bin"},
    {"present_back_buffer", "first_in_frame", "color_%05d.bin"},
    {"render_target_to_shader_resource", "first_in_frame", "normals_%05d.bin"},
    {"render_target_to_shader_resource", "first_in_frame", "specular_%05d.bin"},
};

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

// The first burst's start time for this dump directory - set once, on the
// first ever burst-armed transition, matching the dump-directory-wide (not
// per-burst) scope everything else about the dump layout already has.
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
    // Which surface profile identification ran under (Phase 9), and where it
    // came from: "embedded" means the DLL's own copy of profiles/ue5.json, so
    // producer.commit pins it; anything else is an MV_PROFILE_FILE path, which
    // the commit does not. A dump only exists if velocity was identified,
    // which needs a loaded profile, so null here would be a bug.
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
        const StaticSurfaceFacts& facts = kStaticFacts[i];
        j += std::string("    ") + JsonString(kSurfaceSpecs[i].name == std::string("back buffer") ? "color"
                                                                                                    : kSurfaceSpecs[i].name) +
             ": {\n";
        j += "      \"present\": " + std::string(layout.known ? "true" : "false") + ",\n";
        if (!layout.known && i == kSurfaceDepth)
        {
            j += std::string("      \"reason\": ") +
                 (DepthCaptureEnabled() ? "\"never_identified\"" : "\"capture_disabled\"") + ",\n";
        }
        if (!layout.known && i >= kFirstOptionalSurface)
        {
            // A first-class state, not a log line (docs/REFACTOR_PLAN.md sec
            // 4.1): a consumer can tell "off" from "on but never found".
            const std::string name = kSurfaceSpecs[i].name;
            const SurfaceProfile* profile = ActiveSurfaceProfile();
            // auto: this file has its own TU-local SurfaceSpec (capture-side facts).
            const auto* spec = profile != nullptr ? profile->Optional(name.c_str()) : nullptr;
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
        j += std::string("      \"file_pattern\": ") + JsonString(facts.filePattern);
        if (i >= kFirstOptionalSurface)
        {
            // Unverified against engine source for every optional surface
            // (normals: octahedral vs direct, sec 3.2 #6; specular: GBufferB
            // channel packing). Saying "unknown" is what lets mvtools.py
            // refuse to decode instead of producing a plausible field from a
            // guess.
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
            // Depth's copy rides the game's command list exactly like
            // velocity's, but nothing tallies whether the fence actually
            // covers it - docs/REFACTOR_PLAN.md finding 1.2. Naming that
            // honestly here, rather than reporting fabricated zeros a reader
            // could mistake for "measured and found safe".
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
        const std::string name = (i == kSurfaceColor) ? "color" : kSurfaceSpecs[i].name;
        layoutSurfaces += "        " + JsonString(name) + ": " + LayoutSurfaceJson(g_dumpLayout[i], 8);
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

// Builds and enqueues the manifest write. Always goes through the write
// queue (bypassing backpressure, like frames.csv's header truncation) as the
// LAST job for everything already enqueued - see EnqueueWrite's renameTo
// handling and this file's header comment for why FIFO ordering alone is
// what makes the atomicity guarantee hold, with no additional
// synchronisation.
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

// Maps a finished slot's readback buffers and hands their bytes to the
// writer. All of a frame's artefacts are enqueued together or not at all -
// per-blob backpressure produced frames missing only depth or only colour
// with nothing distinguishing that from "never identified".
bool DrainSlot(Slot& slot)
{
    // Byte counts are already known from GetCopyableFootprints (see
    // EnsureReadback), so the room check runs BEFORE any Map() call rather
    // than after. A frame that cannot fit is dropped before its ~19MB is
    // memcpy'd out of GPU-visible memory for nothing - see
    // docs/REFACTOR_PLAN.md finding 1.3.
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
    // a slot without both is not a capture. Kept explicit rather than folded
    // into the loop because those two are load-bearing in a way the others are
    // not - the `required` distinction in docs/REFACTOR_PLAN.md, which Phase 8
    // turns into a mask frozen at burst arm.
    std::vector<uint8_t> data[kMaxSurfaces];
    bool mapped[kMaxSurfaces] = {};
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

    // Every mapped blob, plus the frames.csv line, is handed to EnqueueFrame
    // together - one atomic unit, so a fourth surface is a table entry here
    // and nowhere gets to enqueue only part of a frame.
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
        sprintf_s(nameBuf, "%s_%05d.bin", kSurfaceSpecs[i].filePrefix, slot.captureIndex);
        jobs.push_back(WriteJob{DumpDir() + nameBuf, std::move(data[i]), /*append=*/false, /*renameTo=*/{}});
        haveVelocity = haveVelocity || i == kSurfaceVelocity;
        haveColor = haveColor || i == kSurfaceColor;
    }

    // Records which GAME frame this capture came from. Capture indices just
    // count what we drained, so adjacent indices aren't necessarily adjacent
    // frames (busy slot, dropped write, second burst) - warp validation needs
    // N-1/N to actually be one frame apart. Included only when both halves of
    // the pair are part of this same atomic enqueue.
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
            // MV_AUTOCAPTURE exists for testhost's debug-layer harness, which has
            // no one to press F8. Off unless the env var is set.
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

// The slot this frame's copies go into: the one already recording for this
// frame, or a fresh one. Either velocity or depth may arrive first (depth
// transitions out of DEPTH_WRITE before velocity finishes), so whichever
// arrives first allocates.
//
// Caller must hold g_mutex.
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
            // Per-surface state resets in one loop, so a fourth surface cannot
            // be left carrying the previous frame's flags. The readback buffer
            // and its footprints deliberately survive - reusing them is the
            // point of the ring, and EnsureReadback revalidates the layout
            // before every copy anyway.
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

} // namespace

void OnVelocityReadable(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* velocity, int stateAfter)
{
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0 || cmdList == nullptr || velocity == nullptr)
    {
        return;
    }
    if (g_originalResourceBarrier == nullptr)
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
    // One velocity copy per frame; the buffer transitions twice per frame and
    // we only want the RENDER_TARGET -> ALL_SHADER_RESOURCE edge.
    if (slot != nullptr && slot->surfaces[kSurfaceVelocity].present)
    {
        return;
    }
    if (slot == nullptr)
    {
        // All slots in flight; skip rather than stall. Designed behaviour, but
        // indistinguishable from a wedged ring without logging - a capture
        // path that can stop silently and take F8 with it is worse than one
        // that stalls loudly.
        const uint64_t misses = g_noFreeSlot.fetch_add(1, std::memory_order_relaxed) + 1;
        if (misses <= 3 || misses % 240 == 0)
        {
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
                std::to_string(g_framesRemaining.load(std::memory_order_relaxed)) +
                " frames still requested) - slots=[" + states +
                "] fenceCompleted=" + std::to_string(g_fence != nullptr ? g_fence->GetCompletedValue() : 0) +
                " nextFence=" + std::to_string(g_nextFenceValue));
        }
        return;
    }

    const D3D12_RESOURCE_DESC desc = velocity->GetDesc();
    if (!DumpLayoutAccepts(kSurfaceVelocity, desc))
    {
        return;
    }
    SurfaceCapture& surface = slot->surfaces[kSurfaceVelocity];
    const bool firstVelocityUse = surface.readback == nullptr;
    if (!EnsureReadback(surface, kSurfaceVelocity, desc))
    {
        return;
    }
    if (firstVelocityUse)
    {
        SnapshotDumpLayout(kSurfaceVelocity, surface);
    }

    RecordCopyToReadback(
        cmdList,
        velocity,
        surface.readback.Get(),
        surface.footprints,
        static_cast<D3D12_RESOURCE_STATES>(stateAfter),
        g_originalResourceBarrier);

    // Remember which command list this copy went onto so fence coverage can
    // be checked, not assumed (see NoteSubmission). Stored as COM identity,
    // not the raw interface pointer (see ComIdentity).
    surface.recordedList = ComIdentity(cmdList);
    surface.coverage = Coverage::None;
    surface.queueSubmissionsAtRecord = g_queueSubmissions.load(std::memory_order_relaxed);
    ++surface.copies;
    surface.present = true;
}

void OnDepthReadable(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* depth, int stateAfter)
{
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0 || cmdList == nullptr || depth == nullptr)
    {
        return;
    }
    if (!DepthCaptureEnabled())
    {
        return;
    }
    if (g_originalResourceBarrier == nullptr)
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
    if (slot == nullptr || slot->surfaces[kSurfaceDepth].copies >= kMaxDepthCopiesPerFrame)
    {
        // No slot free means this frame is being skipped anyway; the velocity
        // path logs that case in detail and there is nothing to add here.
        return;
    }
    if (slot->surfaces[kSurfaceVelocity].present && slot->surfaces[kSurfaceDepth].copies > 0)
    {
        // Stop once velocity has been recorded, so the dump gets the LAST
        // depth edge BEFORE velocity became readable - the depth the frame's
        // velocity was rendered against. Also caps bandwidth: depth copies
        // can be the bulk of per-frame write volume.
        //
        // The `copies > 0` half is a fallback for titles where depth
        // becomes readable AFTER velocity (frame graph ordered differently
        // than UE5's default prepass/base-pass) - without it such a title
        // would silently capture no depth at all.
        return;
    }
    if (slot->surfaces[kSurfaceVelocity].present && !g_depthAfterVelocityReported.exchange(true))
    {
        Log("capture: this title's depth buffer becomes readable only AFTER the velocity buffer "
            "does, so the depth in the dump is from a pass that ran after velocity was finished. "
            "For opaque geometry that is the same depth; if this title writes depth after its "
            "velocity pass, it is not.");
    }

    const D3D12_RESOURCE_DESC desc = depth->GetDesc();
    if (!DumpLayoutAccepts(kSurfaceDepth, desc))
    {
        return;
    }
    SurfaceCapture& surface = slot->surfaces[kSurfaceDepth];
    const bool firstUse = surface.readback == nullptr;
    if (!EnsureReadback(surface, kSurfaceDepth, desc))
    {
        return;
    }
    if (firstUse)
    {
        SnapshotDumpLayout(kSurfaceDepth, surface);
    }

    RecordCopyToReadback(
        cmdList,
        depth,
        surface.readback.Get(),
        surface.footprints,
        static_cast<D3D12_RESOURCE_STATES>(stateAfter),
        g_originalResourceBarrier);
    // Recorded on the GAME's command list, exactly like velocity, so it needs
    // the same coverage key - it simply never had anywhere to put one.
    surface.recordedList = ComIdentity(cmdList);
    surface.queueSubmissionsAtRecord = g_queueSubmissions.load(std::memory_order_relaxed);
    ++surface.copies;
    surface.present = true;

    // Depth edge count per frame depends on the title's frame graph (full
    // prepass = 1, partial prepass = 2); "last one wins" only means what it
    // says if this is observed rather than assumed. Logged sparingly.
    const uint64_t seen = g_depthEdgeReports.fetch_add(1, std::memory_order_relaxed);
    if (seen < 8)
    {
        Log("capture: depth copy " + std::to_string(surface.copies) + " of frame " + std::to_string(frame) +
            " recorded (stateAfter=" + std::to_string(stateAfter) +
            "). The last copy of a frame is the one that lands in the dump.");
    }
}

int OptionalSurfaceIndex(const char* name)
{
    for (int i = kFirstOptionalSurface; i < kSurfaceCount; ++i)
    {
        if (std::strcmp(kSurfaceSpecs[i].name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

void OnOptionalSurfaceReadable(
    const char* surfaceName, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, int stateAfter)
{
    if (g_framesRemaining.load(std::memory_order_relaxed) <= 0 || cmdList == nullptr || resource == nullptr)
    {
        return;
    }
    if (g_originalResourceBarrier == nullptr)
    {
        return;
    }
    const int index = surfaceName != nullptr ? OptionalSurfaceIndex(surfaceName) : -1;
    if (index < 0)
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
    // first_in_frame, one copy: the first RT -> SRV edge is the base pass
    // finishing the GBuffer target. No slot means the frame is skipped anyway
    // (the velocity path logs that).
    if (slot == nullptr || slot->surfaces[index].present)
    {
        return;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (!DumpLayoutAccepts(index, desc))
    {
        return;
    }
    SurfaceCapture& surface = slot->surfaces[index];
    const bool firstUse = surface.readback == nullptr;
    if (!EnsureReadback(surface, index, desc))
    {
        return;
    }
    if (firstUse)
    {
        SnapshotDumpLayout(index, surface);
    }

    RecordCopyToReadback(
        cmdList,
        resource,
        surface.readback.Get(),
        surface.footprints,
        static_cast<D3D12_RESOURCE_STATES>(stateAfter),
        g_originalResourceBarrier);
    // On the game's list like depth, and like depth not graded for fence
    // coverage yet (docs/REFACTOR_PLAN.md finding 1.2) - the manifest says so.
    surface.recordedList = ComIdentity(cmdList);
    surface.queueSubmissionsAtRecord = g_queueSubmissions.load(std::memory_order_relaxed);
    ++surface.copies;
    surface.present = true;
}

void NoteSubmission(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists)
{
    // Cheap early-out: runs on the render thread for every submission; nothing
    // to check outside a capture burst.
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
        // Velocity only, for now: this reshape moves the key into the surface
        // that owns it but deliberately does not change what is graded, so the
        // dump and the burst summary stay identical. Grading depth as well -
        // its copy also rides the game's list and has never been checked - is a
        // visible behaviour change and belongs in its own commit.
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
                // Readback correctness rests on one fence covering both our
                // copy and the velocity copy, which only holds if velocity
                // was submitted to the same queue. A second DIRECT queue
                // would silently produce stale reads.
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
    // velocity is unconditionally required (capture.cpp:1810-ish drops any
    // frame without it, always); depth's requirement is frozen per burst -
    // see g_depthRequiredForBurst.
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
// DIAGNOSTIC (Phase 10): MV_SNAPSHOT=<surface>. Copies ONE frame of each
// resource that surface has shortlisted at the scene extent - at its own
// RT -> SRV edge, exactly as a capture would - and writes it as
// snapshot_<surface>_<ptr>_<w>x<h>_fmt<f>_pitch<p>.bin in the dump
// directory. For telling candidates apart by their pixels when every
// barrier-level signal ties them (specular on Skyrunner: GBufferB vs
// GBufferC and five more B8G8R8A8_TYPELESS targets). It identifies nothing
// and changes no selection. Copies are recorded on the game's command list;
// the fence is signalled on the game's queue at a LATER Present, by which
// time that list has been submitted. Guarded by g_mutex.
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
        // Frozen once, here, not re-read live per frame - see the field
        // comment (finding 1.14).
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
    // still-draining) burst is left in flight. Placed right after the drain
    // loop above, which is what can make DumpFullyDrained() newly true.
    //
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
    // Every captured frame must carry every artefact this session can produce.
    //
    // Partial frames cluster at burst edges: F8 arrives mid-frame, after
    // depth edges but before the velocity edge. Dropping those beats keeping
    // them - a dump with inconsistent frames forces every offline tool to
    // special-case it, and getting that wrong means comparing against the
    // wrong frame silently.
    //
    // Gates on the artefact having been produced at least once, so a title
    // that never identifies depth still captures velocity rather than nothing.
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
    // Known-source exclusion is fed by d3d12_hook.cpp's NoteKnownSources on
    // every Present, burst or not - it used to be fed only here, which only
    // ran mid-burst (see that function for what that broke).

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

    // The fence signalled below covers the velocity copy only if the game has
    // already submitted the list it was recorded onto. NoteSubmission checks
    // this rather than assuming it, so an unmet case is reported instead of
    // silently producing a stale readback.
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
    // Our list, our submit, immediately before the Signal below - so there is
    // nothing to check and nothing that could make it stale. Recorded as its
    // own value rather than a Strong that would mean something different.
    //
    // The assert is the tie between two facts that happen to coincide for
    // colour today but are not the same fact: SourceKind says WHERE the
    // resource's identity came from (an API contract vs. a search),
    // Coverage says whether the FENCE covers this particular copy. Nothing
    // stops a future KnownByConstruction surface from riding a command list
    // it did not submit itself and needing real coverage grading - if that
    // ever happens, this line is where it would need to change, and the
    // assert is what makes silently forgetting to change it loud instead of
    // quiet.
    assert(kSurfaceSpecs[kSurfaceColor].source == SourceKind::KnownByConstruction);
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
        // No-op if an early-end site already recorded a reason this same
        // burst; otherwise this frame is what finally exhausted the
        // requested count.
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
        // Detach rather than join. By DLL_PROCESS_DETACH, Windows has already
        // terminated every other thread, so there's nothing to wait for - but
        // a still-joinable std::thread destructor calls std::terminate(), and
        // this is a file-scope object whose destructor runs during CRT
        // teardown next.
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
    // On a live flush (processExiting == false, from MvFlushCapture) the
    // queue is drained and the thread joined. On process exit it can't be:
    // by DLL_PROCESS_DETACH, Windows has already terminated every other
    // thread, so the writer is gone regardless and joining would only
    // confirm that.
    //
    // The real guarantee is the "write queue drained" line the writer emits
    // when empty - that, not "burst recorded", means it's safe to close the
    // game (documented in README).
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
