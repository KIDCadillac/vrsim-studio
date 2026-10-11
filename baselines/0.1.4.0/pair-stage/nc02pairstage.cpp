/*
 * NC02 dual-slot D3D9 pair staging helper - implementation.
 *
 * Windows / D3D9 / C++17. Single active context per process, pinned to the
 * UI thread of Create. All COM work runs on that thread; the registry
 * SRWLOCK is only ever held for pointer/busy bookkeeping, never across a
 * D3D call. Every C++ exception is caught at the ABI boundary (-9).
 */
#define NC02_PAIR_STAGE_EXPORTS
#include "NC02PairStage.h"

#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <new>

static_assert(D3DFMT_A8R8G8B8 == 21, "accepted format 21");
static_assert(D3DFMT_X8R8G8B8 == 22, "accepted format 22");

namespace {

constexpr uint32_t kMinDim = 2u;
constexpr uint32_t kMaxDim = 2048u;
constexpr uint64_t kMaxEyeBytes = 32ull << 20;         // 32 MiB per eye
constexpr uint64_t kMaxTotalStagingBytes = 64ull << 20; // 4 staging surfaces total

// Free -> CopyIssued -> Ready -> Free (per pair slot).
enum SlotState : uint32_t { kFree = 0, kCopyIssued = 1, kReady = 2, kFaulted = 3 };

struct Slot {
    uint32_t state = kFree;
    NC02PairTokenV1 token{};
    uint64_t leftId = 0;
    uint64_t rightId = 0;
    uint64_t submitQpc = 0;
    uint64_t getRtTicks = 0;
    uint64_t issueTicks = 0;
    uint64_t submitTotalTicks = 0;
    uint64_t queryTicks = 0;
    uint64_t lockTicks = 0;
    uint64_t rowCopyTicks = 0;
    uint64_t unlockTicks = 0;
    uint64_t pollTotalTicks = 0;
    uint32_t queryAttempts = 0;
    uint32_t leftLockAttempts = 0;
    uint32_t rightLockAttempts = 0;
    bool pairValid = false;  // false => drain-only, never publishable
};

struct Context {
    uint64_t id = 0;
    DWORD threadId = 0;
    uint64_t epoch = 0;
    uint64_t generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint64_t rowBytes = 0;   // width * 4
    uint64_t eyeBytes = 0;   // rowBytes * height
    IDirect3DDevice9* device = nullptr;   // owned ref from source GetDevice
    uint64_t deviceIdentity = 0;
    IDirect3DSurface9* staging[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};
    IDirect3DQuery9* queries[2] = {nullptr, nullptr};
    Slot slots[2];
    uint64_t lastSeq = 0;
    int32_t admissionOpen = 0;
    int32_t stopRequested = 0;
    int32_t faulted = 0;
    int32_t unverifiedFault = 0;  // in-flight unknown: Destroy forbidden
    int32_t deviceLost = 0;
    int32_t stopVerified = 0;
    int32_t lastError = 0;
    bool hasLastError = false;
    uint32_t ownedRefs = 0;
    uint64_t createTicks = 0;
    uint64_t releaseTicks = 0;  // cumulative across every StopStep
};

SRWLOCK g_registryLock = SRWLOCK_INIT;
Context* g_context = nullptr;
uint64_t g_nextId = 1;   // IDs start at 1 and are never reused
bool g_callBusy = false;

inline uint64_t QpcNow()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

inline bool IsDeviceLost(HRESULT hr)
{
    return hr == D3DERR_DEVICELOST;
}

void SetStickyError(Context& c, HRESULT hr)
{
    if (!c.hasLastError) {
        c.lastError = static_cast<int32_t>(hr);
        c.hasLastError = true;
    }
}

uint32_t PendingSlots(const Context& c)
{
    uint32_t n = 0;
    if (c.slots[0].state != kFree) ++n;
    if (c.slots[1].state != kFree) ++n;
    return n;
}

// Acquire the single-call slot and validate the handle/thread. The registry
// lock is released before any D3D work. NC02_E_BUSY means a call is already
// active (only reachable via reentry on the owner thread, or a foreign
// thread that fails the thread check first).
int32_t CallBegin(uint64_t handle, Context** out)
{
    AcquireSRWLockExclusive(&g_registryLock);
    int32_t code = NC02_E_HANDLE;
    Context* c = g_context;
    if (c == nullptr) {
        code = NC02_E_HANDLE;
    } else if (c->id != handle) {
        code = NC02_E_HANDLE;
    } else if (c->threadId != GetCurrentThreadId()) {
        code = NC02_E_THREAD;  // no D3D, no pixel writes, ever
    } else if (g_callBusy) {
        code = NC02_E_BUSY;
    } else {
        g_callBusy = true;
        *out = c;
        code = 0;
    }
    ReleaseSRWLockExclusive(&g_registryLock);
    return code;
}

void CallEnd()
{
    AcquireSRWLockExclusive(&g_registryLock);
    g_callBusy = false;
    ReleaseSRWLockExclusive(&g_registryLock);
}

// Destroy uses this so registry removal and busy-clear are one step.
void CallEndAndRemove(Context* c)
{
    AcquireSRWLockExclusive(&g_registryLock);
    if (g_context == c) {
        g_context = nullptr;  // id consumed; g_nextId never goes backwards
    }
    g_callBusy = false;
    ReleaseSRWLockExclusive(&g_registryLock);
}

struct CallGuard {
    ~CallGuard() { CallEnd(); }
};

bool AcceptedFormat(uint32_t f)
{
    return f == static_cast<uint32_t>(D3DFMT_A8R8G8B8) ||
           f == static_cast<uint32_t>(D3DFMT_X8R8G8B8);
}

bool RangeWraps(const void* p, uint64_t len)
{
    const uint64_t a = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
    return len > (0xFFFFFFFFFFFFFFFFull - a);
}

bool RangesOverlap(uint64_t a, uint64_t al, uint64_t b, uint64_t bl)
{
    return a < b + bl && b < a + al;
}

// Strict source-shape gate, identical criteria for Create and Submit.
int32_t CheckSourceDesc(IDirect3DSurface9* src, const Context& c, D3DSURFACE_DESC* outDesc)
{
    const HRESULT hr = src->GetDesc(outDesc);
    if (FAILED(hr)) {
        return static_cast<int32_t>(hr);
    }
    const bool ok =
        outDesc->Width == c.width &&
        outDesc->Height == c.height &&
        outDesc->Format == static_cast<D3DFORMAT>(c.format) &&
        outDesc->Type == D3DRTYPE_SURFACE &&
        outDesc->Pool == D3DPOOL_DEFAULT &&
        outDesc->Usage == D3DUSAGE_RENDERTARGET &&
        outDesc->MultiSampleType == D3DMULTISAMPLE_NONE &&
        outDesc->MultiSampleQuality == 0;
    return ok ? 0 : NC02_E_UNSUPPORTED;
}

// Temporary GetDevice ref must name the owned device; released same turn.
int32_t CheckSourceDevice(IDirect3DSurface9* src, Context& c)
{
    IDirect3DDevice9* dev = nullptr;
    const HRESULT hr = src->GetDevice(&dev);
    if (FAILED(hr)) {
        if (dev != nullptr) dev->Release();
        return static_cast<int32_t>(hr);
    }
    if (dev == nullptr) {
        return NC02_E_FAULT;
    }
    const bool same = (dev == c.device);
    dev->Release();
    return same ? 0 : NC02_E_UNSUPPORTED;
}

// Conservative fence after a partially issued pair: close admission, keep
// the slot drain-only, never free/reuse immediately.
void FencePartialPair(Context& c, uint32_t idx, HRESULT hr)
{
    Slot& s = c.slots[idx];
    s.pairValid = false;  // drain-only
    s.state = kCopyIssued;
    c.admissionOpen = 0;
    c.faulted = 1;  // GetRT failure is a real failure, sticky
    const bool lost = IsDeviceLost(hr);
    if (lost) {
        c.deviceLost = 1;  // never lose the GetRT-proven lost state
    }
    SetStickyError(c, hr);
    IDirect3DQuery9* q = c.queries[idx];
    if (q != nullptr) {
        const uint64_t t0 = QpcNow();
        const HRESULT hrIssue = q->Issue(D3DISSUE_END);
        s.issueTicks += QpcNow() - t0;
        if (FAILED(hrIssue)) {
            c.faulted = 1;
            SetStickyError(c, hrIssue);
            if (IsDeviceLost(hrIssue)) {
                c.deviceLost = 1;
            } else if (!lost) {
                // Live device, unknown issue outcome: unverified retain.
                // Never treat it as drained.
                c.unverifiedFault = 1;
            }
        }
    }
}

void ReleaseOwnedReverse(Context& c, uint64_t* releaseTicksOut)
{
    const uint64_t t0 = releaseTicksOut ? QpcNow() : 0;
    for (int i = 1; i >= 0; --i) {
        if (c.queries[i] != nullptr) {
            c.queries[i]->Release();
            c.queries[i] = nullptr;
            --c.ownedRefs;
        }
        for (int e = 1; e >= 0; --e) {
            if (c.staging[i][e] != nullptr) {
                c.staging[i][e]->Release();
                c.staging[i][e] = nullptr;
                --c.ownedRefs;
            }
        }
    }
    if (c.device != nullptr) {
        c.device->Release();
        c.device = nullptr;
        --c.ownedRefs;
    }
    if (releaseTicksOut) {
        *releaseTicksOut = QpcNow() - t0;
    }
}

// Create-time RAII: owns the provisional Context plus every device/staging/
// query reference it adopted. No GPU copy can be in flight during Create, so
// releasing owned refs on any failure or exception is safe.
struct ProvisionalContext {
    Context* c;
    bool published = false;
    explicit ProvisionalContext(Context* p) : c(p) {}
    void disarm() { published = true; }
    ~ProvisionalContext() {
        if (!published && c != nullptr) {
            // No-throw cleanup boundary: a Release that throws must not escape
            // the destructor (that would terminate the process). Swallow it and
            // keep the unproven refs leaked rather than fake a clean teardown;
            // the context memory itself is always reclaimed.
            try {
                ReleaseOwnedReverse(*c, nullptr);
            } catch (...) {
                // refs left unverified on purpose; no clean result claimed
            }
            delete c;
        }
    }
};

void FillStop(Context& c, NC02StopStateV1* r, uint64_t releaseTicks, uint32_t callBusy)
{
    r->structBytes = NC02_STOP_BYTES;
    r->version = 1;
    r->admissionOpen = c.admissionOpen;
    r->callBusy = callBusy;  // this Stop's own guard is not pending busy on return
    r->pendingSlots = PendingSlots(c);
    r->liveOwnedResourceRefs = c.ownedRefs;
    r->stopVerified = c.stopVerified;
    r->faulted = c.faulted;
    r->lastError = c.lastError;
    r->reserved = 0;
    r->createTicks = c.createTicks;
    r->releaseTicks = releaseTicks;
}

}  // namespace

extern "C" {

int32_t NC02_PairStage_CreateV1(
    void* borrowedSource,
    uint32_t expectedW,
    uint32_t expectedH,
    uint32_t expectedFormat,
    uint64_t epoch,
    uint64_t generation,
    uint64_t* outHandle)
{
    if (borrowedSource == nullptr || outHandle == nullptr) {
        return NC02_E_INVALIDARG;
    }
    if (expectedW < kMinDim || expectedW > kMaxDim ||
        expectedH < kMinDim || expectedH > kMaxDim) {
        return NC02_E_INVALIDARG;
    }
    if (!AcceptedFormat(expectedFormat)) {
        return NC02_E_UNSUPPORTED;
    }
    const uint64_t rowBytes = static_cast<uint64_t>(expectedW) * 4ull;
    if (rowBytes / 4ull != expectedW) {
        return NC02_E_INVALIDARG;
    }
    const uint64_t eyeBytes = rowBytes * static_cast<uint64_t>(expectedH);
    if (eyeBytes / rowBytes != expectedH || eyeBytes > kMaxEyeBytes) {
        return NC02_E_INVALIDARG;
    }
    if (eyeBytes * 4ull > kMaxTotalStagingBytes) {
        return NC02_E_INVALIDARG;
    }

    const uint64_t createStart = QpcNow();
    try {
        // Reserve the single call slot before touching D3D: two concurrent
        // Creates must not both reach the device.
        AcquireSRWLockExclusive(&g_registryLock);
        if (g_context != nullptr || g_callBusy) {
            ReleaseSRWLockExclusive(&g_registryLock);
            return NC02_E_BUSY;
        }
        g_callBusy = true;
        ReleaseSRWLockExclusive(&g_registryLock);
        struct CreateBusyGuard {
            ~CreateBusyGuard() {
                AcquireSRWLockExclusive(&g_registryLock);
                g_callBusy = false;
                ReleaseSRWLockExclusive(&g_registryLock);
            }
        } busyGuard;

        IDirect3DSurface9* src = static_cast<IDirect3DSurface9*>(borrowedSource);

        // Validate the real borrowed source; it is never AddRef'd/stored.
        D3DSURFACE_DESC desc{};
        HRESULT hr = src->GetDesc(&desc);
        if (FAILED(hr)) {
            return static_cast<int32_t>(hr);
        }
        const bool descOk =
            desc.Width == expectedW &&
            desc.Height == expectedH &&
            desc.Format == static_cast<D3DFORMAT>(expectedFormat) &&
            desc.Type == D3DRTYPE_SURFACE &&
            desc.Pool == D3DPOOL_DEFAULT &&
            desc.Usage == D3DUSAGE_RENDERTARGET &&
            desc.MultiSampleType == D3DMULTISAMPLE_NONE &&
            desc.MultiSampleQuality == 0;
        if (!descOk) {
            return NC02_E_UNSUPPORTED;
        }

        // Owned device ref comes from the actual source.
        IDirect3DDevice9* dev = nullptr;
        hr = src->GetDevice(&dev);
        if (FAILED(hr) || dev == nullptr) {
            if (dev != nullptr) dev->Release();
            return FAILED(hr) ? static_cast<int32_t>(hr) : NC02_E_FAULT;
        }

        Context* c = new (std::nothrow) Context();
        if (c == nullptr) {
            dev->Release();
            return NC02_E_INVALIDARG;
        }
        ProvisionalContext owner(c);  // unwinds every owned ref + the Context
        c->device = dev;
        ++c->ownedRefs;  // device ref is tracked like every owned resource
        c->deviceIdentity = reinterpret_cast<uint64_t>(dev);
        c->threadId = GetCurrentThreadId();
        c->epoch = epoch;
        c->generation = generation;
        c->width = expectedW;
        c->height = expectedH;
        c->format = expectedFormat;
        c->rowBytes = rowBytes;
        c->eyeBytes = eyeBytes;

        // Prebuild 4 same-size/format SYSTEMMEM offscreen surfaces + 2 EVENT
        // queries. A non-null output from a failed call is still adopted so
        // the owner guard releases it; no GPU copies exist yet, so immediate
        // release on any failure/exception is safe. No publish on failure.
        bool built = true;
        for (uint32_t s = 0; s < 2 && built; ++s) {
            for (uint32_t e = 0; e < 2; ++e) {
                IDirect3DSurface9* st = nullptr;
                hr = dev->CreateOffscreenPlainSurface(
                    expectedW, expectedH,
                    static_cast<D3DFORMAT>(expectedFormat),
                    D3DPOOL_SYSTEMMEM, &st, nullptr);
                if (st != nullptr) {
                    c->staging[s][e] = st;
                    ++c->ownedRefs;
                }
                if (FAILED(hr) || st == nullptr) {
                    built = false;
                    SetStickyError(*c, FAILED(hr) ? hr : E_FAIL);
                    break;
                }
            }
        }
        for (uint32_t s = 0; s < 2 && built; ++s) {
            IDirect3DQuery9* q = nullptr;
            // Official two-parameter form; EVENT queries carry no data.
            hr = dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
            if (q != nullptr) {
                c->queries[s] = q;
                ++c->ownedRefs;
            }
            if (FAILED(hr) || q == nullptr) {
                built = false;
                SetStickyError(*c, FAILED(hr) ? hr : E_FAIL);
                break;
            }
        }
        if (!built) {
            return c->hasLastError ? c->lastError : NC02_E_UNSUPPORTED;
        }

        c->id = 0;  // assigned under the registry lock below
        c->createTicks = QpcNow() - createStart;

        // Publish ID and pointer atomically. IDs increase forever; an
        // exhausted counter is rejected and never reused.
        AcquireSRWLockExclusive(&g_registryLock);
        if (g_context != nullptr) {
            ReleaseSRWLockExclusive(&g_registryLock);
            return NC02_E_BUSY;  // lost a race against another publish
        }
        if (g_nextId == UINT64_MAX) {
            ReleaseSRWLockExclusive(&g_registryLock);
            return NC02_E_BUSY;  // handle space exhausted; no ID reuse, ever
        }
        c->id = g_nextId++;
        c->admissionOpen = 1;
        g_context = c;
        owner.disarm();
        ReleaseSRWLockExclusive(&g_registryLock);

        *outHandle = c->id;
        return 0;
    } catch (...) {
        return NC02_E_CPP_EXCEPTION;
    }
}

int32_t NC02_PairStage_SubmitPairV1(
    uint64_t handle,
    void* borrowedLeft,
    void* borrowedRight,
    const NC02PairTokenV1* token,
    uint32_t tokenBytes)
{
    Context* raw = nullptr;
    const int32_t enter = CallBegin(handle, &raw);
    if (enter != 0) {
        return enter;  // no D3D, no output for rejected calls
    }
    CallGuard guard;
    Context& c = *raw;

    try {
        // Whole-call timing starts at the real entry, so GetDesc/GetDevice
        // validation is included in submitTotalTicks.
        const uint64_t submitStart = QpcNow();

        // ---- freeze the caller token before any D3D work (TOCTOU) ----
        if (token == nullptr || tokenBytes != NC02_PAIR_TOKEN_BYTES) {
            return NC02_E_TOKEN;
        }
        NC02PairTokenV1 frozen;
        std::memcpy(&frozen, token, sizeof(frozen));  // exactly 40 bytes
        if (frozen.structBytes != NC02_PAIR_TOKEN_BYTES || frozen.version != 1) {
            return NC02_E_TOKEN;
        }

        // ---- argument / state gates before any slot mutation ----
        if (borrowedLeft == nullptr || borrowedRight == nullptr ||
            borrowedLeft == borrowedRight) {
            return NC02_E_INVALIDARG;
        }
        if (c.stopRequested) {
            return NC02_E_STOPPED;
        }
        if (c.faulted || c.unverifiedFault) {
            return NC02_E_FAULT;
        }
        if (!c.admissionOpen) {
            return NC02_E_STOPPED;
        }
        if (frozen.epoch != c.epoch || frozen.ownerGeneration != c.generation) {
            return NC02_E_TOKEN;
        }
        if (frozen.pairSeq == 0 || frozen.pairSeq <= c.lastSeq) {
            return NC02_E_TOKEN;
        }

        uint32_t idx;
        if (c.slots[0].state == kFree) {
            idx = 0;
        } else if (c.slots[1].state == kFree) {
            idx = 1;
        } else {
            return 1;  // Full: never overwrite or block
        }

        // ---- strict validation of both borrowed sources (sync use only) ----
        IDirect3DSurface9* srcL = static_cast<IDirect3DSurface9*>(borrowedLeft);
        IDirect3DSurface9* srcR = static_cast<IDirect3DSurface9*>(borrowedRight);
        D3DSURFACE_DESC dL{}, dR{};
        int32_t code = CheckSourceDesc(srcL, c, &dL);
        if (code != 0) return code;
        code = CheckSourceDesc(srcR, c, &dR);
        if (code != 0) return code;
        code = CheckSourceDevice(srcL, c);
        if (code != 0) return code;
        code = CheckSourceDevice(srcR, c);
        if (code != 0) return code;

        // ---- adopt the pair: frozen local only, never re-read caller ----
        Slot& s = c.slots[idx];
        s.token = frozen;
        s.leftId = reinterpret_cast<uint64_t>(borrowedLeft);
        s.rightId = reinterpret_cast<uint64_t>(borrowedRight);
        s.submitQpc = QpcNow();
        s.getRtTicks = 0;
        s.issueTicks = 0;
        s.queryTicks = 0;
        s.lockTicks = 0;
        s.rowCopyTicks = 0;
        s.unlockTicks = 0;
        s.pollTotalTicks = 0;
        s.queryAttempts = 0;
        s.leftLockAttempts = 0;
        s.rightLockAttempts = 0;
        s.submitTotalTicks = 0;
        s.pairValid = true;
        s.state = kCopyIssued;

        // GetRT left, then right; borrowed sources used only in this stack.
        const uint64_t tL = QpcNow();
        HRESULT hr = c.device->GetRenderTargetData(srcL, c.staging[idx][0]);
        s.getRtTicks += QpcNow() - tL;
        if (FAILED(hr)) {
            FencePartialPair(c, idx, hr);
            return static_cast<int32_t>(hr);
        }
        const uint64_t tR = QpcNow();
        hr = c.device->GetRenderTargetData(srcR, c.staging[idx][1]);
        s.getRtTicks += QpcNow() - tR;
        if (FAILED(hr)) {
            // Left copy may have reached the GPU: fence, drain, do not free.
            FencePartialPair(c, idx, hr);
            return static_cast<int32_t>(hr);
        }

        // EVENT query: only Issue(END) is legal; arm it after the copies.
        IDirect3DQuery9* q = c.queries[idx];
        const uint64_t tIssue1 = QpcNow();
        hr = q->Issue(D3DISSUE_END);
        s.issueTicks += QpcNow() - tIssue1;
        if (FAILED(hr)) {
            s.pairValid = false;
            s.state = kCopyIssued;  // in-flight unknown; hold everything
            c.admissionOpen = 0;
            c.faulted = 1;
            SetStickyError(c, hr);
            if (IsDeviceLost(hr)) {
                c.deviceLost = 1;
            } else {
                c.unverifiedFault = 1;  // Destroy forbidden from here
            }
            return static_cast<int32_t>(hr);
        }

        // Kick this PAIR event once while the owned-context admission is still open.
        // A single FLUSH submits queued driver work; it is not a wait loop or publication.
        if (c.stopRequested || !c.admissionOpen) {
            s.pairValid = false;
            return NC02_E_STOPPED;
        }
        if (c.faulted || c.unverifiedFault) {
            s.pairValid = false;
            return NC02_E_FAULT;
        }
        const HRESULT kickHr = q->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        if (FAILED(kickHr)) {
            c.faulted = 1;
            c.admissionOpen = 0;
            SetStickyError(c, kickHr);
            s.pairValid = false;
            if (IsDeviceLost(kickHr)) {
                c.deviceLost = 1;
                s.state = kFree; // same proven-dead-device abandon rule as PollCollect
            } else {
                s.state = kFaulted;
                c.unverifiedFault = 1; // live-device query outcome unknown: retain, Destroy forbidden
            }
            return static_cast<int32_t>(kickHr);
        }
        if (kickHr != S_OK && kickHr != S_FALSE) {
            c.faulted = 1;
            c.unverifiedFault = 1;
            c.admissionOpen = 0;
            SetStickyError(c, kickHr);
            s.pairValid = false;
            s.state = kFaulted;
            return NC02_E_FAULT; // unexpected positive is not completion or acceptance
        }
        // Both S_OK and S_FALSE keep kCopyIssued. Original PollCollect remains the
        // authoritative ready/lock/copy gate and still uses FLUSH, never flag 0.
        // Poll/Stop queryAttempts/queryTicks and Issue-only issueTicks stay unchanged;
        // this single Submit kick's host cost is included by submitTotalTicks below.

        s.submitTotalTicks = QpcNow() - submitStart;

        // ---- final stop/fault gate after the successful Issue(END) ----
        // A reentrant StopStep may have closed the doors during this call.
        // The copies and armed query are real: keep the slot kCopyIssued with
        // pairValid=false for StopStep's bounded drain; do not free/release
        // and do not advance lastSeq. Same priority as Poll's late gate.
        if (c.stopRequested || !c.admissionOpen) {
            s.pairValid = false;
            return NC02_E_STOPPED;
        }
        if (c.faulted) {
            s.pairValid = false;
            return NC02_E_FAULT;
        }

        c.lastSeq = frozen.pairSeq;  // accepted; monotonic gate for next Submit
        return 0;
    } catch (...) {
        c.faulted = 1;
        c.admissionOpen = 0;
        c.unverifiedFault = 1;  // call state unknown; Destroy forbidden
        SetStickyError(c, E_FAIL);
        return NC02_E_CPP_EXCEPTION;
    }
}

int32_t NC02_PairStage_PollCollectV1(
    uint64_t handle,
    uint8_t* outL,
    uint64_t capL,
    uint8_t* outR,
    uint64_t capR,
    NC02CollectedPairV1* result,
    uint32_t resultCapacity)
{
    Context* raw = nullptr;
    const int32_t enter = CallBegin(handle, &raw);
    if (enter != 0) {
        return enter;
    }
    CallGuard guard;
    Context& c = *raw;

    IDirect3DSurface9* heldL = nullptr;  // outstanding locks for exception paths
    IDirect3DSurface9* heldR = nullptr;

    try {
        // ---- capacity/alias checks before touching anything ----
        if (result == nullptr || resultCapacity != NC02_COLLECTED_BYTES ||
            outL == nullptr || outR == nullptr) {
            return NC02_E_INVALIDARG;
        }
        if (capL < c.eyeBytes || capR < c.eyeBytes ||
            RangeWraps(outL, capL) || RangeWraps(outR, capR) ||
            RangeWraps(result, NC02_COLLECTED_BYTES)) {
            return NC02_E_INVALIDARG;
        }
        const uint64_t a = reinterpret_cast<uintptr_t>(outL);
        const uint64_t b = reinterpret_cast<uintptr_t>(outR);
        const uint64_t r = reinterpret_cast<uintptr_t>(result);
        if (RangesOverlap(a, c.eyeBytes, b, c.eyeBytes) ||
            RangesOverlap(a, capL, r, NC02_COLLECTED_BYTES) ||
            RangesOverlap(b, capR, r, NC02_COLLECTED_BYTES)) {
            return NC02_E_INVALIDARG;  // buffers must not alias
        }

        // ---- stop / fault gates before any GetData, lock or copy ----
        // Stop drains only via StopStepV1; Poll publishes nothing once a
        // stop was requested or the context faulted.
        if (c.faulted || c.unverifiedFault) {
            return NC02_E_FAULT;
        }
        if (c.stopRequested || !c.admissionOpen) {
            return NC02_E_STOPPED;
        }

        const uint64_t pollStart = QpcNow();

        // ---- oldest accepted pair only ----
        int idx = -1;
        uint64_t best = 0;
        for (int i = 0; i < 2; ++i) {
            if (c.slots[i].state != kFree &&
                (idx < 0 || c.slots[i].token.pairSeq < best)) {
                idx = i;
                best = c.slots[i].token.pairSeq;
            }
        }
        if (idx < 0) {
            return 1;  // Empty; result/pixels untouched
        }
        Slot& s = c.slots[static_cast<uint32_t>(idx)];
        if (s.state == kFaulted || !s.pairValid) {
            // Unpublishable pair; StopStep drops it once provably drained.
            return c.hasLastError ? c.lastError : NC02_E_FAULT;
        }

        auto pollTime = [&](void) { s.pollTotalTicks += QpcNow() - pollStart; };

        // ---- one bounded FLUSH GetData per call, no loops/waits ----
        if (s.state == kCopyIssued) {
            ++s.queryAttempts;
            const uint64_t t0 = QpcNow();
            const HRESULT hr = c.queries[static_cast<uint32_t>(idx)]
                                   ->GetData(nullptr, 0, D3DGETDATA_FLUSH);
            s.queryTicks += QpcNow() - t0;
            if (hr == S_FALSE) {
                pollTime();
                return 1;  // Pending
            }
            if (FAILED(hr)) {
                c.faulted = 1;
                c.admissionOpen = 0;
                SetStickyError(c, hr);
                s.pairValid = false;
                if (IsDeviceLost(hr)) {
                    c.deviceLost = 1;
                    s.state = kFree;  // dead device: abandon, fault stays
                } else {
                    // Live device, unknown query outcome: unverified retain;
                    // no new Submit is admitted from here on.
                    s.state = kFaulted;
                    c.unverifiedFault = 1;
                }
                pollTime();
                return static_cast<int32_t>(hr);
            }
            if (hr != S_OK) {
                // Unexpected positive (neither S_OK nor S_FALSE): unknown query
                // outcome. Never fake completion; close doors and retain.
                c.faulted = 1;
                c.unverifiedFault = 1;
                c.admissionOpen = 0;
                SetStickyError(c, hr);
                s.pairValid = false;
                s.state = kFaulted;
                pollTime();
                return NC02_E_FAULT;  // -8, does not impersonate done
            }
            s.state = kReady;
        }

        // ---- READONLY|DONOTWAIT locks; zero pixel writes until both hold ----
        D3DLOCKED_RECT lrL{}, lrR{};
        ++s.leftLockAttempts;
        const uint64_t tlL = QpcNow();
        HRESULT hr = c.staging[idx][0]->LockRect(&lrL, nullptr,
                                                 D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
        s.lockTicks += QpcNow() - tlL;
        if (FAILED(hr)) {
            if (hr == D3DERR_WASSTILLDRAWING) {
                pollTime();
                return 1;  // left busy: nothing written, retry next UI turn
            }
            // Real lock failure with no lock held and the query already proven
            // ready: sticky-fault, close admission, but keep the slot kReady
            // (not kFaulted) so StopStep can safely drop it during drain.
            c.faulted = 1;
            c.admissionOpen = 0;
            SetStickyError(c, hr);
            s.pairValid = false;
            s.state = kReady;
            pollTime();
            return static_cast<int32_t>(hr);  // real HRESULT, no blocking fallback
        }
        heldL = c.staging[idx][0];

        ++s.rightLockAttempts;
        const uint64_t tlR = QpcNow();
        const HRESULT hrR = c.staging[idx][1]->LockRect(&lrR, nullptr,
                                                        D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
        s.lockTicks += QpcNow() - tlR;
        if (FAILED(hrR)) {
            // Release the left lock first; copy nothing this round.
            const uint64_t tu = QpcNow();
            const HRESULT hrU = c.staging[idx][0]->UnlockRect();
            s.unlockTicks += QpcNow() - tu;
            if (SUCCEEDED(hrU)) {
                heldL = nullptr;
            }
            if (FAILED(hrU)) {
                s.state = kFaulted;
                s.pairValid = false;
                c.faulted = 1;
                c.unverifiedFault = 1;
                c.admissionOpen = 0;
                SetStickyError(c, hrU);
                pollTime();
                return static_cast<int32_t>(hrU);
            }
            if (hrR == D3DERR_WASSTILLDRAWING) {
                pollTime();
                return 1;  // right busy: no pixels written, retry next turn
            }
            // Non-busy right-lock failure with the left already unlocked and no
            // lock outstanding: sticky-fault, close admission, keep slot kReady
            // for a safe Stop drop, return the real HRESULT.
            c.faulted = 1;
            c.admissionOpen = 0;
            SetStickyError(c, hrR);
            s.pairValid = false;
            s.state = kReady;
            pollTime();
            return static_cast<int32_t>(hrR);
        }
        heldR = c.staging[idx][1];

        if (lrL.Pitch <= 0 || lrR.Pitch <= 0 ||
            static_cast<uint64_t>(lrL.Pitch) < c.rowBytes ||
            static_cast<uint64_t>(lrR.Pitch) < c.rowBytes ||
            lrL.pBits == nullptr || lrR.pBits == nullptr) {
            const uint64_t tu = QpcNow();
            const HRESULT hrUL = c.staging[idx][0]->UnlockRect();
            const HRESULT hrUR = c.staging[idx][1]->UnlockRect();
            s.unlockTicks += QpcNow() - tu;
            heldL = nullptr;
            heldR = nullptr;
            if (FAILED(hrUL) || FAILED(hrUR)) {
                s.state = kFaulted;
                s.pairValid = false;
                c.faulted = 1;
                c.unverifiedFault = 1;
                c.admissionOpen = 0;
                SetStickyError(c, FAILED(hrUL) ? hrUL : hrUR);
                pollTime();
                return static_cast<int32_t>(FAILED(hrUL) ? hrUL : hrUR);
            }
            // Shape rejected after both unlocks succeeded; nothing copied.
            // Sticky-fault and close admission, keep the slot kReady for a
            // safe Stop drop (no lock outstanding).
            c.faulted = 1;
            c.admissionOpen = 0;
            SetStickyError(c, NC02_E_UNSUPPORTED);
            s.pairValid = false;
            s.state = kReady;
            pollTime();
            return NC02_E_UNSUPPORTED;  // shape rejected, nothing copied
        }

        // ---- row copy of raw BGRA including alpha, no gamma ----
        const uint64_t tc = QpcNow();
        const uint8_t* srcLp = static_cast<const uint8_t*>(lrL.pBits);
        uint8_t* dstL = outL;
        for (uint32_t y = 0; y < c.height; ++y) {
            std::memcpy(dstL + static_cast<uint64_t>(y) * c.rowBytes,
                        srcLp + static_cast<uint64_t>(y) * lrL.Pitch,
                        c.rowBytes);
        }
        const uint8_t* srcRp = static_cast<const uint8_t*>(lrR.pBits);
        uint8_t* dstR = outR;
        for (uint32_t y = 0; y < c.height; ++y) {
            std::memcpy(dstR + static_cast<uint64_t>(y) * c.rowBytes,
                        srcRp + static_cast<uint64_t>(y) * lrR.Pitch,
                        c.rowBytes);
        }
        s.rowCopyTicks += QpcNow() - tc;

        const uint64_t tu = QpcNow();
        const HRESULT hrUL = c.staging[idx][0]->UnlockRect();
        const HRESULT hrUR = c.staging[idx][1]->UnlockRect();
        s.unlockTicks += QpcNow() - tu;
        heldL = nullptr;
        heldR = nullptr;
        if (FAILED(hrUL) || FAILED(hrUR)) {
            // Pixels above were already written; the whole pair is
            // unpublishable and must not be reported Complete.
            const HRESULT bad = FAILED(hrUL) ? hrUL : hrUR;
            s.state = kFaulted;
            s.pairValid = false;
            c.faulted = 1;
            c.unverifiedFault = 1;
            c.admissionOpen = 0;
            SetStickyError(c, bad);
            pollTime();
            return static_cast<int32_t>(bad);
        }

        // ---- late stop/fault gate before publishing ----
        // Pixels were copied and both surfaces unlocked cleanly, but a Stop or
        // a fault may have been recorded in the meantime. The whole pair is
        // then invalid: free the slot, write no result, never claim Complete.
        if (c.stopRequested || !c.admissionOpen) {
            s.state = kFree;
            s.pairValid = false;
            pollTime();
            return NC02_E_STOPPED;  // pixels may already be written here
        }
        if (c.faulted || c.unverifiedFault) {
            s.state = kFree;
            s.pairValid = false;
            pollTime();
            return NC02_E_FAULT;
        }

        // ---- publish Complete with the frozen token ----
        const uint64_t now = QpcNow();
        s.pollTotalTicks += now - pollStart;
        std::memset(result, 0, NC02_COLLECTED_BYTES);
        result->token = s.token;
        result->slotIndex = static_cast<uint32_t>(idx);
        result->status = NC02_COLLECTED_STATUS_COMPLETE;
        result->leftSourceIdentity = s.leftId;
        result->rightSourceIdentity = s.rightId;
        result->deviceIdentity = c.deviceIdentity;
        result->width = c.width;
        result->height = c.height;
        result->submitQpc = s.submitQpc;
        result->collectQpc = now;
        result->getRenderTargetDataTicks = s.getRtTicks;
        result->issueTicks = s.issueTicks;
        result->queryTicks = s.queryTicks;
        result->lockTicks = s.lockTicks;
        result->rowCopyTicks = s.rowCopyTicks;
        result->unlockTicks = s.unlockTicks;
        result->submitTotalTicks = s.submitTotalTicks;
        result->pollTotalTicks = s.pollTotalTicks;
        result->queryAttempts = s.queryAttempts;
        result->leftLockAttempts = s.leftLockAttempts;
        result->rightLockAttempts = s.rightLockAttempts;
        result->reserved = 0;

        s.state = kFree;
        s.pairValid = false;
        return 0;
    } catch (...) {
        // Best-effort unlock so no staging lock survives this call. Each
        // UnlockRect is individually guarded so a second C++ exception cannot
        // escape the catch and terminate the process.
        if (heldR != nullptr) {
            try { heldR->UnlockRect(); } catch (...) {}
        }
        if (heldL != nullptr) {
            try { heldL->UnlockRect(); } catch (...) {}
        }
        c.faulted = 1;
        c.admissionOpen = 0;
        c.unverifiedFault = 1;  // a lock may be outstanding; Destroy forbidden
        SetStickyError(c, E_FAIL);
        return NC02_E_CPP_EXCEPTION;
    }
}

int32_t NC02_PairStage_StopStepV1(uint64_t handle, NC02StopStateV1* result, uint32_t resultCapacity)
{
    if (result == nullptr || resultCapacity < NC02_STOP_BYTES) {
        return NC02_E_INVALIDARG;
    }

    Context* raw = nullptr;
    const int32_t enter = CallBegin(handle, &raw);
    if (enter == NC02_E_BUSY) {
        // Reentrant Stop from the owning thread while another call holds the
        // context. Do not touch D3D, do not Release, and never delete the busy
        // context. Under a short registry lock, re-check the handle is live on
        // this thread; only then flip the door flags and snapshot busy.
        Context* busyCtx = nullptr;
        int32_t code = NC02_E_BUSY;
        AcquireSRWLockExclusive(&g_registryLock);
        Context* cand = g_context;
        if (cand == nullptr || cand->id != handle) {
            code = NC02_E_HANDLE;
        } else if (cand->threadId != GetCurrentThreadId()) {
            code = NC02_E_THREAD;  // wrong thread: reject unchanged, doors stay
        } else {
            cand->admissionOpen = 0;
            cand->stopRequested = 1;
            busyCtx = cand;
        }
        ReleaseSRWLockExclusive(&g_registryLock);
        if (code != NC02_E_BUSY) {
            return code;  // -5 / -4, nothing else touched
        }
        // Snapshot busy (callBusy=1, no D3D performed) and report Busy/Pending.
        FillStop(*busyCtx, result, busyCtx->releaseTicks, 1);
        return 1;
    }
    if (enter != 0) {
        return enter;
    }
    CallGuard guard;
    Context& c = *raw;

    try {
        c.admissionOpen = 0;  // closed first; nothing further is published
        c.stopRequested = 1;

        if (c.stopVerified) {
            // Idempotent: report the accumulated release timing, not 0 ticks.
            FillStop(c, result, c.releaseTicks, 0);
            return 0;
        }
        if (c.unverifiedFault) {
            // In-flight work unknown: keep every owned resource. Managed
            // caller owns the 32-poll/500ms bound; native never busy-waits.
            FillStop(c, result, c.releaseTicks, 0);
            return 1;
        }

        // ---- finite drain: at most one FLUSH GetData per pending slot ----
        for (uint32_t i = 0; i < 2; ++i) {
            Slot& s = c.slots[i];
            if (s.state == kCopyIssued) {
                ++s.queryAttempts;
                const uint64_t t0 = QpcNow();
                const HRESULT hr = c.queries[i]->GetData(nullptr, 0, D3DGETDATA_FLUSH);
                s.queryTicks += QpcNow() - t0;
                if (hr == S_FALSE) {
                    FillStop(c, result, c.releaseTicks, 0);
                    return 1;  // still draining
                }
                if (FAILED(hr)) {
                    if (IsDeviceLost(hr)) {
                        c.deviceLost = 1;
                        c.faulted = 1;
                        SetStickyError(c, hr);
                        s.state = kFree;   // abandon, fault stays sticky
                        s.pairValid = false;
                        continue;
                    }
                    // Real negative on a live device: unverified and hold
                    // resources; return the true HRESULT.
                    c.faulted = 1;
                    c.unverifiedFault = 1;
                    SetStickyError(c, hr);
                    FillStop(c, result, c.releaseTicks, 0);
                    return static_cast<int32_t>(hr);
                }
                if (hr != S_OK) {
                    // Unexpected positive (neither S_OK nor S_FALSE): unknown
                    // query outcome. Do not fake readiness; close and retain.
                    c.faulted = 1;
                    c.unverifiedFault = 1;
                    SetStickyError(c, hr);
                    s.pairValid = false;
                    s.state = kFaulted;
                    FillStop(c, result, c.releaseTicks, 0);
                    return NC02_E_FAULT;  // negative fail, not a fake drain
                }
                s.state = kReady;
            }
            if (s.state == kReady || s.state == kFaulted) {
                // Stop never locks/copies pixels: drop the pair.
                if (s.state == kFaulted && !c.deviceLost) {
                    c.faulted = 1;
                    FillStop(c, result, c.releaseTicks, 0);
                    return NC02_E_FAULT;
                }
                s.state = kFree;
                s.pairValid = false;
            }
        }

        // ---- all slots free, no locks, no unverified work: release ----
        // Device-lost cleanup that already has no real unknown lock/copy may
        // release here; the sticky fault is preserved either way.
        uint64_t releaseDelta = 0;
        ReleaseOwnedReverse(c, &releaseDelta);
        c.releaseTicks += releaseDelta;  // cumulative across StopStep calls
        if (c.ownedRefs != 0) {
            // Ref accounting mismatch: keep the context and the timing proof.
            FillStop(c, result, c.releaseTicks, 0);
            return NC02_E_FAULT;
        }
        c.stopVerified = 1;  // faulted stays 1 for device-lost cleanup
        FillStop(c, result, c.releaseTicks, 0);
        return 0;
    } catch (...) {
        c.faulted = 1;
        c.admissionOpen = 0;
        c.unverifiedFault = 1;
        SetStickyError(c, E_FAIL);
        return NC02_E_CPP_EXCEPTION;
    }
}

int32_t NC02_PairStage_DestroyV1(uint64_t handle)
{
    Context* raw = nullptr;
    const int32_t enter = CallBegin(handle, &raw);
    if (enter != 0) {
        return enter;  // stale/foreign/busy handles are rejected, nothing freed
    }

    Context& c = *raw;
    try {
        if (c.unverifiedFault) {
            CallEnd();
            return NC02_E_FAULT;  // Destroy forbidden until in-flight is known
        }
        if (!c.stopVerified || c.ownedRefs != 0 || PendingSlots(c) != 0) {
            CallEnd();
            return NC02_E_STOPPED;  // StopStep must verify drain first
        }

        // Detach first so no other lookup can see the context, then finish
        // teardown outside the lock. CallEndAndRemove also clears busy.
        CallEndAndRemove(&c);
        delete &c;
        return 0;
    } catch (...) {
        CallEnd();
        return NC02_E_CPP_EXCEPTION;
    }
}

}  // extern "C"
