// SharedStereoBridge.cpp -- CARD3A: VRSIM::SharedStereoBridge implementation.
//
// Reuses the Gate2-proven helpers by ABSOLUTE-path #include of the pinned
// ObsD3D9ActualSourceQA.cpp (SHA FFCE39922F90D4A5760395A132D4C94E6531B63EA03B
// 2AC1F333F02D3CC35CFF; that file includes the audited Gate1, which includes
// frozen Gate0). The included files are NOT modified here. Helpers reused:
//   g2::AcquireSources, g2::InitProbeAndGpu, g2::FlagOpenLeaf2,
//   g2::FpuPreserve, g2::WaitEvent9NoPump, g2::Eye, g2::FlagOut2,
//   g2::ReleaseEye, g1::Ctx, g1::DiscoverLayout, g1::Cleanup,
//   g1::MAX_POLLS / g1::DEADLINE_MS / g1::FMT88 / g1::FMT22, ReadBytesSafe.
// NEVER called (readback/QA-only, must not become the production loop):
//   ObsSourcePairGate2V1 (the inherited dllexport), g2::Run, g2::RunEye,
//   g1::Run, g1::FillCopy, g1::WaitEvent9/WaitEvent11 (pumping variants),
//   g1::InitDevicesAndAdapter, g1::MakeShared11, g1::EmitJson, either wmain.
// A later build-freeze root will redirect the include to the pinned frozen
// helpers; this TU edits nothing outside the two new Card3A files.
//
// P1 corrections in this revision:
//  1) BusyAdopt ADOPTS a flag the entry already CAS-acquired exactly once;
//     the guard clears it on every return/unwind (no second CAS, no stuck).
//  2) CloseAdmission and the final closed-check + slot-state grant of
//     Prepare/Commit/BeginRead share one short grant lock (no D3D/wait).
//  3) Unified SRWLOCK lifetime gate: Pimpl published once under exclusive,
//     NEVER deleted; Finalize phase 1 exclusive gate + finalizing flag; COM
//     Release/driver work happens only outside locks.
//  4) Whole SourceUI D3D transactions (INIT incl. bootstrap identity QI and
//     final own-ref cleanup; PREPARE incl. both StretchRect paths and
//     cleanup; FINALIZE incl. all Releases/Cleanup) run inside one
//     g2::FpuPreserve scope whose REAL capture + restore-verify results
//     gate the HRESULT. Unproven capture/restore => admission closed +
//     quarantine + non-zero. No ignored restore return values.
//  5) initialized_==0 never claims clean: cleanupProven_ requires a fully
//     proven pre-GPU-phase release (releasedAll + verified restore), else
//     quarantine/retain + non-zero; failed Init never fabricates success.
//
// SCOPE LIMIT: standalone bridge class; NOT wired into XR, no producer loop,
// no FPS claim. Per-frame paths (Prepare/BeginRead/EndRead/Poll) perform no
// JSON, no vectors, no pixel buffers and no GPU allocation; everything is
// allocated exactly once in InitializeOnSourceUI.
#ifndef _M_X64
#error CARD3A SharedStereoBridge requires x64
#endif

#include "SharedStereoBridge.h"

#ifndef UNICODE
#define UNICODE
#endif
#include "shared-helpers/ObsD3D9ActualSourceQA.cpp"

#include <new>

namespace VRSIM {
namespace {

// ---- small pure helpers -----------------------------------------------------
constexpr LONG kFree = (LONG)kSharedStereoFree;
constexpr LONG kPreparing = (LONG)kSharedStereoPreparing;
constexpr LONG kPrepared = (LONG)kSharedStereoPrepared;
constexpr LONG kCommitted = (LONG)kSharedStereoCommitted;
constexpr LONG kReading = (LONG)kSharedStereoReading;
constexpr LONG kInflight = (LONG)kSharedStereoInflight;

inline bool LuidEq(const LUID& a, const LUID& b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

// Canonical COM identity via QI(IID_IUnknown) only; never raw-pointer guesses.
inline bool CanonicalEq(IUnknown* a, IUnknown* b)
{
    if (!a || !b) return false;
    IUnknown* ua = nullptr;
    IUnknown* ub = nullptr;
    const bool ok = SUCCEEDED(a->QueryInterface(IID_IUnknown, (void**)&ua)) && ua &&
                    SUCCEEDED(b->QueryInterface(IID_IUnknown, (void**)&ub)) && ub && ua == ub;
    if (ua) ua->Release();
    if (ub) ub->Release();
    return ok;
}

// Actual adapter descriptor behind any D3D11 device (no fallback, no create).
bool AdapterDescOf(ID3D11Device* dev, DXGI_ADAPTER_DESC& out)
{
    if (!dev) return false;
    IDXGIDevice* gd = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&gd)) || !gd) return false;
    IDXGIAdapter* ga = nullptr;
    const HRESULT h1 = gd->GetAdapter(&ga);
    gd->Release();
    if (FAILED(h1) || !ga) return false;
    const HRESULT h2 = ga->GetDesc(&out);
    ga->Release();
    return SUCCEEDED(h2);
}

inline void SafeRelease(IUnknown** p)
{
    if (*p) { (*p)->Release(); *p = nullptr; }
}

// Strict monotonic ticket allocation. Overflow (wrap or sign hit) fails
// closed: the caller must quarantine; tickets are never reused.
inline bool NextTicketMonotonic(volatile LONG64* ctr, uint64_t* out)
{
    const LONG64 v = InterlockedIncrement64(ctr);
    if (v <= 0) return false;
    *out = (uint64_t)v;
    return true;
}

// Busy guard ADOPT: the entry must have successfully CAS-acquired the flag
// BEFORE constructing this guard; the destructor then clears it exactly once
// on every return/unwind path. No second CAS here (that was P1-1).
struct BusyAdopt {
    volatile LONG* flag;
    explicit BusyAdopt(volatile LONG* f) : flag(f) {}
    ~BusyAdopt() noexcept { InterlockedExchange(flag, 0); }
    BusyAdopt(const BusyAdopt&) = delete;
    BusyAdopt& operator=(const BusyAdopt&) = delete;
};

// RAII over the shared/exclusive halves of the unified lifetime gate.
struct GateShared {
    SRWLOCK* lock;
    explicit GateShared(SRWLOCK* l) : lock(l) { AcquireSRWLockShared(lock); }
    ~GateShared() noexcept { ReleaseSRWLockShared(lock); }
    GateShared(const GateShared&) = delete;
    GateShared& operator=(const GateShared&) = delete;
};
struct GateExclusive {
    SRWLOCK* lock;
    explicit GateExclusive(SRWLOCK* l) : lock(l) { AcquireSRWLockExclusive(lock); }
    ~GateExclusive() noexcept { ReleaseSRWLockExclusive(lock); }
    GateExclusive(const GateExclusive&) = delete;
    GateExclusive& operator=(const GateExclusive&) = delete;
};

// Never-blocking exclusive attempt for the finalize gate: the short gates are
// held by other entries only for pure-atomic windows, but finalize must never
// queue unboundedly behind them -- failure to grab the gate immediately maps
// to S_FALSE (retry later).
struct GateTryExclusive {
    SRWLOCK* lock;
    bool held;
    explicit GateTryExclusive(SRWLOCK* l)
        : lock(l), held(TryAcquireSRWLockExclusive(l) != FALSE) {}
    ~GateTryExclusive() noexcept { if (held) ReleaseSRWLockExclusive(lock); }
    GateTryExclusive(const GateTryExclusive&) = delete;
    GateTryExclusive& operator=(const GateTryExclusive&) = delete;
};

// Active-entry registration RAII: Enter() is called under the SHORT lifetime
// gate right after the stable Impl is confirmed; the count drops exactly once
// on any scope exit, including exception unwinds, so a throwing entry can
// never leave the finalize gate permanently blocked.
struct EntryGuard {
    volatile LONG* count = nullptr;
    EntryGuard() noexcept = default; // deleted copy ctor suppresses the implicit default one
    void Enter(volatile LONG* c) { count = c; InterlockedIncrement(c); }
    ~EntryGuard() noexcept { if (count != nullptr) InterlockedDecrement(count); }
    EntryGuard(const EntryGuard&) = delete;
    EntryGuard& operator=(const EntryGuard&) = delete;
};

} // namespace
#if defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING)
// ---- QA-only device-local D9 GPU timing observer state (nested guard:
// VRSIM_QA_D9_BEGIN_POLL + VRSIM_QA_D9_GPU_TIMING) ----------------------------
// Two preallocated slots created on the ACTUAL source device ctx_.ord during
// the original InitializeOnSourceUI; each owns four query objects
// (TIMESTAMP begin/end, TIMESTAMPDISJOINT, TIMESTAMPFREQ) and observes ONLY
// the real right-eye StretchRect. COM pointers and the per-transaction draft
// are mutated on the source UI thread only while the per-call sourceBusy_
// guard + FPU capture are held; cross-thread visibility is via the Interlocked
// numeric mirrors read by FillQaObsCache. If either macro is undefined none
// of this exists and the original code is byte-for-byte unchanged.
namespace qaobs {
enum SlotStateV : LONG { kFree = 0, kBuilding = 1, kPending = 2, kRetired = 3 };
enum IssueBitV : uint32_t {
    kIssuedFreq = 1u, kIssuedDisjointBegin = 2u, kIssuedBegin = 4u,
    kIssuedEnd = 8u, kIssuedDisjointEnd = 16u
};
enum DataBitV : uint32_t { kDataFreq = 1u, kDataBegin = 2u, kDataEnd = 4u, kDataDisjoint = 8u };
// Resolve field order (finite cursor): 0 freq, 1 begin, 2 end, 3 disjoint.
enum FieldV : uint32_t { kFieldFreq = 0u, kFieldBegin = 1u, kFieldEnd = 2u, kFieldDisjoint = 3u, kFieldCount = 4u };
} // namespace qaobs

struct QaD9ObsSlot {
    LONG state = qaobs::kFree;           // source-thread only (busy = skip, no wait)
    uint32_t issueMask = 0u;             // real successful Issue bits (qaobs::IssueBitV)
    IDirect3DQuery9* freq = nullptr;     // TIMESTAMPFREQ
    IDirect3DQuery9* beginTs = nullptr;  // TIMESTAMP before the right copy
    IDirect3DQuery9* endTs = nullptr;    // TIMESTAMP after the right copy
    IDirect3DQuery9* disjoint = nullptr; // TIMESTAMPDISJOINT bracket
    VRSIM::QaD9GpuObsRecordV1 draft{};   // this transaction's scratch (never published)
};
#endif // VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_D9_GPU_TIMING

// =============================================================================
// Impl: owns everything. Allocated and published only by
// InitializeOnSourceUI under the exclusive lifetime gate; NEVER deleted on
// any path (failed Init, quarantine, finalized): the tiny pure tracking
// object is retained until process exit so cached queries and published
// out-params can never dangle. GPU/COM refs are released only on a proven
// path; otherwise quarantined and retained.
// =============================================================================
class SharedStereoBridge::Impl {
public:
    explicit Impl(volatile LONG* admission) noexcept
        : admission_(admission)
    {
        for (uint32_t i = 0; i < kStereoBridgeSlotCountV1; ++i) {
            state_[i] = kFree;
            slotTicket_[i] = 0;
            everRead_[i] = 0;
            xrEv_[i] = nullptr;
        }
        InitializeSRWLock(&grantLock_);
    }

    bool Quarantined() noexcept { return InterlockedCompareExchange(&quarantined_, 0, 0) != 0; }
    // P#5: initialized_/finalized_/finalizing_/cleanupProven_ are volatile
    // LONG published ONLY via Interlocked, so lock-free phase-2 writes and
    // concurrent readers never form a data race.
    bool InitializedNow() { return InterlockedCompareExchange(&initialized_, 0, 0) != 0; }

    // Final-state publication after phase 2, called by the wrapper under the
    // SHORT exclusive gate (P#2/P#5).
    void PublishFinalized() { InterlockedExchange(&finalized_, 1); }

    // P#2: active GPU/init entries registered under the SHORT lifetime gate.
    // The finalize grant requires this to be 0 (the finalize caller's own
    // sourceBusy_ adoption is legal and excluded here).
    volatile LONG activeEntries_ = 0;

    // ---- gates ----------------------------------------------------------------
    bool Closed() const { return InterlockedCompareExchange(admission_, 0, 0) != 0; }
    // LiveUsable excludes finalizing_ so no NEW entry can race Finalize
    // phase 2 (which runs outside any lock).
    bool LiveUsable()
    {
        return InterlockedCompareExchange(&initialized_, 0, 0) != 0 &&
               InterlockedCompareExchange(&finalized_, 0, 0) == 0 &&
               InterlockedCompareExchange(&finalizing_, 0, 0) == 0 && !Quarantined();
    }

    // P2: first-claim is an atomic CAS so two racing first callers can never
    // overwrite each other's binding; a claimer that loses sees the winner's
    // tid and is refused. Later calls must match exactly.
    bool FixOrMatchSourceThread()
    {
        const LONG t = (LONG)GetCurrentThreadId();
        const LONG prev = InterlockedCompareExchange(&sourceThread_, t, 0);
        return prev == 0 || prev == t;
    }
    bool FixOrMatchWorkerThread()
    {
        const LONG src = InterlockedCompareExchange(&sourceThread_, 0, 0);
        if (src == 0) return false; // worker gate requires an established owner
        const LONG t = (LONG)GetCurrentThreadId();
        if (t == src) return false; // worker must differ from source UI
        const LONG prev = InterlockedCompareExchange(&workerThread_, t, 0);
        return prev == 0 || prev == t;
    }

    void MarkQuarantined()
    {
        InterlockedExchange(admission_, 1);
        const LONG prev = InterlockedExchange(&quarantined_, 1);
        if (prev == 0) InterlockedIncrement64(&cQuarantine_);
    }

    // Called only from an outer catch-all: quarantine and drop the busy flag
    // an in-flight Finalize may still hold (every other method's BusyAdopt
    // released during unwinding before reaching that catch).
    void QuarantineAfterThrow()
    {
        MarkQuarantined();
        InterlockedExchange(&sourceBusy_, 0);
    }

    // ---- CloseAdmission (shared lifetime gate held by the caller) ----------
    // Sets the flag, then briefly takes the grant lock exclusively so no
    // grant can sit between its closed-check and its state CAS: any grant
    // that already committed is visible/drainable; none can start after.
    HRESULT CloseAdmissionLocked()
    {
        const LONG prev = InterlockedCompareExchange(admission_, 1, 0);
        AcquireSRWLockExclusive(&grantLock_);
        ReleaseSRWLockExclusive(&grantLock_);
        return prev == 0 ? S_OK : S_FALSE;
    }

    // ---- Initialize (runs OUTSIDE the lifetime gate as one tracked entry) --
    HRESULT Initialize(ID3D11Device* xrDevice, ID3D11DeviceContext* xrImmediateContext,
                       const LUID& runtimeRequiredLuid, uint32_t width, uint32_t height,
                       uint32_t generation, uint64_t epoch, uint64_t borrowedLeft,
                       uint64_t borrowedRight)
    {
        // Pure parameter validation happens BEFORE any SDK/COM call: on these
        // returns the bridge provably owns nothing, so the empty state is
        // tracked as a proven cleanup (P#6: finalize may then claim S_OK
        // without ever running phase 2 -- a real proof, never a guess).
        if (width != kStereoBridgeWidthV1 || height != kStereoBridgeHeightV1 || generation == 0u ||
            epoch == 0ull || xrDevice == nullptr || xrImmediateContext == nullptr ||
            borrowedLeft == 0ull || borrowedRight == 0ull) {
            InterlockedExchange(&cleanupProven_, 1);
            return E_INVALIDARG;
        }
        // Live-owner / finalize states: refused, and NEVER claimed empty
        // (resources may exist or the owner may already have finalized).
        if (InterlockedCompareExchange(&initialized_, 0, 0) != 0 ||
            InterlockedCompareExchange(&finalized_, 0, 0) != 0 ||
            InterlockedCompareExchange(&finalizing_, 0, 0) != 0)
            return E_ACCESSDENIED;
        // P1 (last review): a brand-new owner cancelled by a concurrent
        // CloseAdmission here -- after parameter validation, before thread
        // binding and the first COM/GPU acquisition -- provably owns nothing:
        // the wrapper's m_impl check guarantees Impl::Initialize runs at most
        // once per owner, and gpuPhaseStarted_ cannot be set yet. Record the
        // tracked-empty proof so TryFinalize succeeds instead of quarantining
        // a resource-free bridge. The refusal HRESULT itself stays unchanged.
        if (Closed()) {
            if (!gpuPhaseStarted_) InterlockedExchange(&cleanupProven_, 1);
            return E_ACCESSDENIED;
        }
        if (!FixOrMatchSourceThread()) return E_ACCESSDENIED;
        if (!sourceBusyAdopt()) return E_FAIL; // P1-1: single acquisition...
        BusyAdopt busyGuard(&sourceBusy_);     // ...adopted here; cleared exactly once

        HRESULT res;
        try {
            res = InitializeInner(xrDevice, xrImmediateContext, runtimeRequiredLuid, width, height,
                                  generation, epoch, borrowedLeft, borrowedRight);
        } catch (...) {
            // Unknown state after a throw: quarantine, retain every ref.
            MarkQuarantined();
            res = E_FAIL;
        }
        return res;
    }

    // P1-4/P1-5: the WHOLE SourceUI D3D transaction is captured before the
    // first SDK/COM call (including GetImmediateContext and the borrowed-
    // surface QIs) and restored+verified only AFTER the final own-ref
    // cleanup; the real restore result gates the outcome. Any failure after
    // the GPU phase quarantines/retains; a pre-GPU-phase failure claims
    // cleanup ONLY if releasedAll AND verified restore AND no unsafe retain,
    // otherwise it quarantines too. Failed Init never returns S_OK.
    HRESULT InitializeInner(ID3D11Device* xrDevice, ID3D11DeviceContext* xrImmediateContext,
                             const LUID& runtimeRequiredLuid, uint32_t width, uint32_t height,
                             uint32_t generation, uint64_t epoch, uint64_t borrowedLeft,
                             uint64_t borrowedRight)
    {
        g2::FpuPreserve fpu;
        fpu.Capture();
        if (!fpu.Captured()) {
            // P#4: an unproven capture quarantines, closes admission and
            // fails non-zero -- the bridge is never left open on unknown
            // FPU state, even though nothing was acquired yet.
            MarkQuarantined();
            return E_FAIL;
        }

        HRESULT res = E_FAIL;
        bool preGpuFail = false;
        g2::Eye eL("init-left"), eR("init-right");

        // Context must be the canonical immediate context of xrDevice.
        ID3D11DeviceContext* got = nullptr;
        xrDevice->GetImmediateContext(&got); // COM call: covered by the capture above
        const bool canonCtx = got != nullptr && CanonicalEq((IUnknown*)got, (IUnknown*)xrImmediateContext);
        SafeRelease((IUnknown**)&got);
        if (!canonCtx) {
            preGpuFail = true; // nothing acquired; own refs = zero
        } else {
            // Stage A: QI own short-lived refs of the borrowed surfaces plus
            // real desc/device/parent gates on the ACTUAL source (Gate2 helper).
            bool surfacesDistinct = false, sameDevice = false, beh044 = false, halType = false,
                 parentOk = false, ordNotExQI = false;
            const bool srcOk =
                g2::AcquireSources(ctx_, borrowedLeft, borrowedRight, eL, eR, surfacesDistinct, sameDevice,
                                   beh044, halType, parentOk, ordNotExQI) &&
                eL.d.Width == width && eL.d.Height == height && eR.d.Width == width && eR.d.Height == height;
            // Stage B: own Ex probe on the actual source ordinal, LUID/adapter
            // proof, and the source-side D3D11 device (Gate2 helper).
            if (srcOk && g2::InitProbeAndGpu(ctx_)) {
                // STRICT four-way same-GPU gate, no fallback:
                // source D9 == source-side D11 == XR device adapter == required.
                DXGI_ADAPTER_DESC srcAd{}, xrAd{};
                bool gpuGate = ctx_.haveLuid && LuidEq(ctx_.luid, runtimeRequiredLuid) &&
                               AdapterDescOf(ctx_.dev11, srcAd) && LuidEq(srcAd.AdapterLuid, ctx_.luid) &&
                               AdapterDescOf(xrDevice, xrAd) && LuidEq(xrAd.AdapterLuid, runtimeRequiredLuid);
                // Same vendor/device/subsystem/revision proven across the matched adapters.
                gpuGate = gpuGate && xrAd.VendorId == ctx_.ad.VendorId && xrAd.DeviceId == ctx_.ad.DeviceId &&
                          xrAd.SubSysId == ctx_.ad.SubSysId && xrAd.Revision == ctx_.ad.Revision;
                if (gpuGate) {
                    // Dynamic layout rediscovery on the ACTUAL parents (audited Gate1).
                    g1::DiscoverLayout(ctx_);
                    if (ctx_.layoutProved) {
                        // Borrowed SDK surface refs do NOT survive this callback.
                        g2::ReleaseEye(eL);
                        g2::ReleaseEye(eR);
                        // GPU allocation phase: from the first source D3D9 API
                        // call on, ANY failure quarantines and retains (never
                        // frees GPU resources across an unproven flag restore
                        // or unknown queue state).
                        gpuPhaseStarted_ = true;
                        res = AllocSharedPool(xrDevice, width, height);
                        if (SUCCEEDED(res)) {
                            generation_ = generation;
                            epoch_ = epoch;
                            width_ = width;
                            height_ = height;
                            xrDevice_ = xrDevice;
                            xrCtx_ = xrImmediateContext;
                            for (uint32_t i = 0; i < kStereoBridgeSlotCountV1; ++i)
                                InterlockedExchange(&state_[i], kFree);
                            InterlockedExchange(&initialized_, 1);
                            InterlockedIncrement64(&cInit_);
#if defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING)
                            // Observer preallocation runs entirely inside the
                            // ORIGINAL InitializeInner FpuPreserve scope and the
                            // original init ownership; unsupported/partial
                            // creation only DISABLES observation, records the
                            // reason and never fails product initialization.
                            QaObsInitCreate();
#endif
                        }
                    } else {
                        preGpuFail = true;
                    }
                } else {
                    preGpuFail = true;
                }
            } else if (FAILED(res) && !gpuPhaseStarted_) {
                preGpuFail = true;
            }

            if (preGpuFail && !gpuPhaseStarted_) {
                // Failed before the source-D3D9 GPU phase: nothing was ever
                // submitted, so every own ref (surfaces, device/parent,
                // probe, modules, window) is released exactly once via the
                // audited cleanup, still inside the FPU capture.
                g2::ReleaseEye(eL);
                g2::ReleaseEye(eR);
                g1::Cleanup(ctx_);
            }
        }

        const bool fpuOk = fpu.RestoreAndVerify(); // real rc decides; never ignored
        if (!fpuOk) {
            MarkQuarantined(); // unproven restore: close admission, retain, fail
            InterlockedExchange(&initialized_, 0);
            res = E_FAIL;
        }
        if (FAILED(res) && !gpuPhaseStarted_) {
            // P1-5: cleanup is only "proven" when the audited release says
            // everything went, nothing was retained and the FPU restore was
            // verified. Otherwise quarantine + retain + non-zero (already).
            const bool prov = fpuOk && ctx_.releasedAll && !ctx_.unsafeRetained;
            InterlockedExchange(&cleanupProven_, prov ? 1 : 0);
            if (!prov) MarkQuarantined();
        } else if (SUCCEEDED(res)) {
            InterlockedExchange(&cleanupProven_, 0); // live owner: finalize governs
        }
        return res;
    }

    // ---- Prepare --------------------------------------------------------------
    // P1-4: one FPU capture covers the whole SourceUI D3D transaction: the
    // bootstrap AcquireSources identity QIs, both StretchRects + drains, the
    // own-ref release and g1::Cleanup; the verified restore result gates the
    // final HRESULT (no ignored restore on the StretchRect-fail returns).
    // P1-2: the final closed check + ticket + state grant are one short
    // grant-lock section; no D3D call or wait inside that lock.
    // The public entry keeps its exact signature semantics: ws defaults to
    // nullptr at the class surface, so existing callsites never sample. This
    // wrapper only zeroes the internal POD and records the REAL returned
    // HRESULT; every branch/error/rollback outcome below stays byte-identical
    // in behavior and a failed Prepare still returns its original HRESULT.
    HRESULT Prepare(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                    SharedPreparedPairV1* out, PrepareWaitSample* ws)
    {
        if (ws != nullptr) std::memset(ws, 0, sizeof(*ws));
        const HRESULT res = PrepareBody(slotIndex, leftSurface, rightSurface, out, ws);
        if (ws != nullptr) {
            ws->bridgeHresult = (LONG)res;
            ws->maskBridgeHresult = 1u;
        }
        return res;
    }

    HRESULT PrepareBody(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                        SharedPreparedPairV1* out, PrepareWaitSample* ws)
    {
        if (out == nullptr) return E_POINTER;
        if (slotIndex >= kStereoBridgeSlotCountV1) return E_INVALIDARG;
        if (leftSurface == 0ull || rightSurface == 0ull) return E_INVALIDARG;
#ifdef VRSIM_QA_D9_BEGIN_POLL
        // QA Begin/Poll collision: there is exactly ONE persistent D9 EVENT
        // (d9Ev_), so the synchronous Prepare must refuse while the pending
        // ledger exists -- no QI, no copy, no Issue, no true-lease mutation.
        // This refusal exists only under the QA define; define-OFF behavior
        // is unchanged. An occupied ledger also blocks finalize separately.
        if (InterlockedCompareExchange(&qaLedgerState_, 0, 0) != 0) return E_FAIL;
#endif
        if (!LiveUsable()) return E_ACCESSDENIED;
        if (Closed()) return E_ACCESSDENIED; // early refuse; final check under grant lock
        if (!FixOrMatchSourceThread()) return E_ACCESSDENIED;
        if (!sourceBusyAdopt()) return E_FAIL;
        BusyAdopt busyGuard(&sourceBusy_);

        // P#3: the final closed check and the FREE->PREPARING start grant are
        // ONE short grant-lock section, exactly like CloseAdmissionLocked's
        // flag-then-drain, so a concurrent Close can never interleave between
        // them. A started (granted) transaction may finish its real GPU work
        // OUTSIDE this lock; its final PREPARED grant re-checks closed.
        const LONG slot = (LONG)slotIndex;
        bool startGranted = false;
        AcquireSRWLockExclusive(&grantLock_);
        if (!Closed() && InterlockedCompareExchange(&state_[slot], kPreparing, kFree) == kFree)
            startGranted = true;
        ReleaseSRWLockExclusive(&grantLock_);
        if (!startGranted) return E_FAIL;

        HRESULT res = E_FAIL;
        g2::FpuPreserve fpu;
        fpu.Capture();
        if (!fpu.Captured()) {
            // P#4: unproven capture quarantines and closes admission (never
            // a silently-open bridge). No COM/D3D9 call was made, so rolling
            // the unsubmitted slot back is safe.
            InterlockedCompareExchange(&state_[slot], kFree, kPreparing);
            MarkQuarantined();
            return E_FAIL;
        }

        bool drained = false;
        bool unprovenWork = false; // real StretchRect submissions whose drain is not proven
        bool ticketOverflow = false;
        g1::Ctx sc; // scratch: own short-lived refs only for this call
        g2::Eye L("prep-left"), R("prep-right");
        try {
            bool surfacesDistinct = false, sameDevice = false, beh044 = false, halType = false,
                 parentOk = false, ordNotExQI = false;
            if (g2::AcquireSources(sc, leftSurface, rightSurface, L, R, surfacesDistinct, sameDevice,
                                   beh044, halType, parentOk, ordNotExQI)) {
                // Exact same canonical actual D3D9 device AND parent as Init retained.
                if (L.d.Width == width_ && L.d.Height == height_ &&
                    R.d.Width == width_ && R.d.Height == height_ &&
                    CanonicalEq((IUnknown*)sc.ord, (IUnknown*)ctx_.ord) &&
                    CanonicalEq((IUnknown*)sc.d9, (IUnknown*)ctx_.d9)) {
                    res = CopyPairIntoSlot(slot, sc, L, R, drained, unprovenWork, ws);
                }
            }

            if (drained) {
                // One short atomic grant section: final Closed() check +
                // monotonic ticket + PREPARING->PREPARED + out fill.
                AcquireSRWLockExclusive(&grantLock_);
                SharedPreparedPairV1 t{};
                bool granted = false;
                if (!Closed()) {
                    uint64_t tk = 0;
                    const bool ticketOk = NextTicketMonotonic(&nextTicket_, &tk);
                    if (!ticketOk) {
                        ticketOverflow = true;
                    } else if (InterlockedCompareExchange(&state_[slot], kPrepared, kPreparing) == kPreparing) {
                        InterlockedExchange64(&slotTicket_[slot], (LONG64)tk);
                        InterlockedExchange(&everRead_[slot], 0);
                        t.version = kStereoBridgeVersionV1;
                        t.structBytes = (uint32_t)sizeof(SharedPreparedPairV1);
                        t.slotIndex = slotIndex;
                        t.generation = generation_;
                        t.epoch = epoch_;
                        t.ticket = tk;
                        t.legacyLeft = legacyNum_[slot][0];
                        t.legacyRight = legacyNum_[slot][1];
                        t.width = width_;
                        t.height = height_;
                        t.format88 = kStereoBridgeFormat88V1;
                        *out = t;
                        granted = true;
                    }
                }
                ReleaseSRWLockExclusive(&grantLock_);
                if (granted) {
                    InterlockedIncrement64(&cPrepare_);
                    res = S_OK;
                } else {
                    res = E_FAIL;
                }
            }

            if (unprovenWork || ticketOverflow || Quarantined()) {
                // Drain after a real submission unproven, or ticket overflow:
                // retain ALL refs (surfaces, scratch device/parent),
                // quarantine, never clean up.
                MarkQuarantined();
                if (SUCCEEDED(res)) res = E_FAIL;
            } else {
                // Proven drain (or pre-submission failure): release every
                // local owned QI exactly once, then roll a failed slot back.
                // Still inside the FPU capture; restore verified afterwards.
                g2::ReleaseEye(L);
                g2::ReleaseEye(R);
                g1::Cleanup(sc);
                if (FAILED(res))
                    InterlockedCompareExchange(&state_[slot], kFree, kPreparing);
            }
        } catch (...) {
            MarkQuarantined();
            res = E_FAIL;
        }

        const bool fpuOk = fpu.RestoreAndVerify(); // real result gates; never ignored
        if (!fpuOk) {
            MarkQuarantined();
            res = E_FAIL;
        }
        return res;
    }

    // ---- Commit / Discard ------------------------------------------------------
    // Commit's final closed check + PREPARED->COMMITTED CAS are one atomic
    // grant-lock section (no D3D calls, so no FPU wrapper needed).
    HRESULT Commit(const SharedPreparedPairV1& t)
    {
        if (!LiveUsable()) return E_ACCESSDENIED;
        if (Closed()) return E_ACCESSDENIED; // early refuse; final check under grant lock
        if (!FixOrMatchSourceThread()) return E_ACCESSDENIED;
        if (!sourceBusyAdopt()) return E_FAIL;
        BusyAdopt busyGuard(&sourceBusy_);
        uint32_t slot = 0;
        if (!TicketIdentityMatches(t, slot)) return E_FAIL;
        HRESULT res = E_FAIL;
        AcquireSRWLockExclusive(&grantLock_);
        if (!Closed() &&
            InterlockedCompareExchange(&state_[slot], kCommitted, kPrepared) == kPrepared) {
            InterlockedIncrement64(&cCommit_);
            res = S_OK;
        }
        ReleaseSRWLockExclusive(&grantLock_);
        return res;
    }

    HRESULT Discard(const SharedPreparedPairV1& t)
    {
        if (!LiveUsable()) return E_ACCESSDENIED;
        if (!FixOrMatchSourceThread()) return E_ACCESSDENIED;
        if (!sourceBusyAdopt()) return E_FAIL;
        BusyAdopt busyGuard(&sourceBusy_);
        uint32_t slot = 0;
        if (!TicketIdentityMatches(t, slot)) return E_FAIL;
        const LONG st = InterlockedCompareExchange(&state_[slot], 0, 0);
        if (st == kPrepared) {
            // PREPARED exists only with the D9 drain already proven in Prepare.
            if (InterlockedCompareExchange(&state_[slot], kFree, kPrepared) != kPrepared) return E_FAIL;
            InterlockedIncrement64(&cDrop_);
            return S_OK;
        }
        if (st == kCommitted && Closed() && InterlockedCompareExchange(&everRead_[slot], 0, 0) == 0) {
            // Drain-only: a never-read COMMITTED ticket may be released under
            // closed admission. READING/INFLIGHT are never reclaimed here.
            if (InterlockedCompareExchange(&state_[slot], kFree, kCommitted) != kCommitted) return E_FAIL;
            InterlockedIncrement64(&cDrop_);
            return S_OK;
        }
        return E_FAIL;
    }

    // ---- Worker side -------------------------------------------------------------
    // BeginRead's final closed check + COMMITTED->READING reservation are one
    // atomic grant-lock section.
    HRESULT BeginRead(const SharedPreparedPairV1& t, ID3D11ShaderResourceView** left,
                      ID3D11ShaderResourceView** right)
    {
        if (left == nullptr || right == nullptr) return E_POINTER;
        if (!LiveUsable()) return E_ACCESSDENIED;
        if (Closed()) return E_ACCESSDENIED; // closed admission prohibits new BeginRead
        if (!FixOrMatchWorkerThread()) return E_ACCESSDENIED;
        if (!workerBusyAdopt()) return E_FAIL;
        BusyAdopt busyGuard(&workerBusy_);
        uint32_t slot = 0;
        if (!TicketIdentityMatches(t, slot)) return E_FAIL;
        HRESULT res = E_FAIL;
        AcquireSRWLockExclusive(&grantLock_);
        if (!Closed() &&
            InterlockedCompareExchange(&state_[slot], kReading, kCommitted) == kCommitted) {
            InterlockedExchange(&everRead_[slot], 1);
            *left = xrSrv_[slot][0]; // borrowed persistent pointers: caller must not Release
            *right = xrSrv_[slot][1];
            res = S_OK;
        }
        ReleaseSRWLockExclusive(&grantLock_);
        return res;
    }

    HRESULT EndRead(const SharedPreparedPairV1& t)
    {
        if (!LiveUsable()) return E_ACCESSDENIED; // drain allowed while closed, not while finalizing
        if (!FixOrMatchWorkerThread()) return E_ACCESSDENIED;
        if (!workerBusyAdopt()) return E_FAIL;
        BusyAdopt busyGuard(&workerBusy_);
        uint32_t slot = 0;
        if (!TicketIdentityMatches(t, slot)) return E_FAIL;
        if (InterlockedCompareExchange(&state_[slot], 0, 0) != kReading) return E_FAIL;
        // Fence after the source reads; safe even if the consumer read nothing
        // after a successful BeginRead. No synchronous readback here.
        xrCtx_->End(xrEv_[slot]);
        xrCtx_->Flush();
        if (InterlockedCompareExchange(&state_[slot], kInflight, kReading) != kReading) {
            MarkQuarantined();
            return E_FAIL;
        }
        return S_OK;
    }

    HRESULT PollCompleted()
    {
        if (!LiveUsable()) return E_ACCESSDENIED;
        if (!FixOrMatchWorkerThread()) return E_ACCESSDENIED;
        if (!workerBusyAdopt()) return E_FAIL;
        BusyAdopt busyGuard(&workerBusy_);
        for (LONG slot = 0; slot < (LONG)kStereoBridgeSlotCountV1; ++slot) {
            if (InterlockedCompareExchange(&state_[slot], 0, 0) != kInflight) continue;
            BOOL signaled = FALSE;
            const HRESULT h = xrCtx_->GetData(xrEv_[slot], &signaled, sizeof(BOOL), 0); // exactly 4-byte Win32 BOOL
            if (h == S_OK) {
                if (signaled == TRUE) {
                    if (InterlockedCompareExchange(&state_[slot], kFree, kInflight) == kInflight)
                        InterlockedIncrement64(&cDrain_);
                } else {
                    MarkQuarantined(); // S_OK+FALSE is not a proven completion
                    return E_FAIL;
                }
            } else if (h == S_FALSE) {
                continue; // pending: no sleep, no pump, return promptly
            } else {
                MarkQuarantined(); // driver error: quarantine and retain
                return h;
            }
        }
        return S_OK;
    }

    // ---- Finalize phase 1 (short exclusive lifetime gate held by caller) -----
    // Pure atomics only: no COM, no D3D, no waits inside the gate. P#6: an
    // already-clean proven-empty failed Init finalizes HERE (needsPhase2
    // stays false, finalized_ published, phase 2 never runs -- no double
    // Cleanup of an object proven empty). A real cleanup grant sets
    // finalizing_ and needsPhase2=true; the caller then runs
    // FinalizeResources() AFTER releasing the gate. The source thread's own
    // busy flag is legitimately 1 here (this call): carried into phase 2 and
    // cleared there. Active entries are counted under the same short gate by
    // every wrapper, so 0 means no GPU transaction can overlap phase 2.
    HRESULT BeginFinalizeGate(bool* needsPhase2)
    {
        *needsPhase2 = false;
        if (Quarantined()) return E_ACCESSDENIED; // unproven state: retain until exit
        if (InterlockedCompareExchange(&finalized_, 0, 0) != 0 ||
            InterlockedCompareExchange(&finalizing_, 0, 0) != 0)
            return E_ACCESSDENIED; // not reusable / in progress
        if (!Closed()) return E_FAIL; // admission must be closed first
        if (!FixOrMatchSourceThread()) return E_ACCESSDENIED;
        if (!sourceBusyAdopt()) return E_FAIL;
        if (InterlockedCompareExchange(&workerBusy_, 0, 0) != 0 ||
            InterlockedCompareExchange(&activeEntries_, 0, 0) != 0) {
            InterlockedExchange(&sourceBusy_, 0);
            return S_FALSE; // a worker or another entry is in flight
        }
        if (InterlockedCompareExchange(&initialized_, 0, 0) == 0) {
            // Never successfully initialized: success ONLY on a tracked,
            // PROVEN cleanup of a tracked-empty acquisition (P1-5/P#6).
            // Unknown refs still quarantine and retain.
            const bool done = InterlockedCompareExchange(&cleanupProven_, 0, 0) != 0;
            if (done) InterlockedExchange(&finalized_, 1);
            else MarkQuarantined();
            InterlockedExchange(&sourceBusy_, 0);
            return done ? S_OK : E_FAIL;
        }
#ifdef VRSIM_QA_D9_BEGIN_POLL
        // QA Begin/Poll: per-call busy/entry zero is NOT an empty-ledger proof.
        // A persistent lease (PREPARING with owned refs, or quarantined
        // retained) explicitly blocks the finalize grant; the slot-state loop
        // below alone is not sufficient because the ledger is the durable
        // pending truth. Quarantined leases already refuse at the top.
        if (InterlockedCompareExchange(&qaLedgerState_, 0, 0) != 0) {
            InterlockedExchange(&sourceBusy_, 0);
            return S_FALSE; // pending ledger occupied: drain/cancel first
        }
#endif
        for (uint32_t i = 0; i < kStereoBridgeSlotCountV1; ++i) {
            if (InterlockedCompareExchange(&state_[i], 0, 0) != kFree) {
                InterlockedExchange(&sourceBusy_, 0);
                return S_FALSE; // still draining
            }
        }
        InterlockedExchange(&finalizing_, 1);
        *needsPhase2 = true;
        return S_OK; // caller now runs FinalizeResources() outside the lock
    }

    // ---- Finalize phase 2 (no lifetime lock; entries blocked by finalizing_) --
    // All slots FREE means every D9 drain (Prepare) and every D11 drain
    // (Poll S_OK+TRUE) was already proven. P1-4: the FPU capture covers every
    // Release here (the last source-side COM work on d3d9.dll) through the
    // audited Cleanup; the verified restore result gates the outcome.
    // Lock-free by design: no COM Release or driver wait happens inside any
    // lock; the exclusive phase-1 gate already excluded in-flight entries.
    HRESULT FinalizeResources()
    {
        HRESULT res = E_FAIL;
        bool captured = false;
        g2::FpuPreserve fpu;
        fpu.Capture();
        captured = fpu.Captured();
        bool releasedAll = false;
        if (captured) {
            bool poolClean = true;
            for (uint32_t i = 0; i < kStereoBridgeSlotCountV1; ++i) {
                SafeRelease((IUnknown**)&xrSrv_[i][0]);
                SafeRelease((IUnknown**)&xrSrv_[i][1]);
                SafeRelease((IUnknown**)&xrTex_[i][0]);
                SafeRelease((IUnknown**)&xrTex_[i][1]);
                SafeRelease((IUnknown**)&sharedSurf9_[i][0]);
                SafeRelease((IUnknown**)&sharedSurf9_[i][1]);
                SafeRelease((IUnknown**)&shared9_[i][0]);
                SafeRelease((IUnknown**)&shared9_[i][1]);
                SafeRelease((IUnknown**)&srcShared11_[i][0]);
                SafeRelease((IUnknown**)&srcShared11_[i][1]);
                legacy_[i][0] = nullptr; // opaque numbers dropped; never closed
                legacy_[i][1] = nullptr;
                legacyNum_[i][0] = 0;
                legacyNum_[i][1] = 0;
                SafeRelease((IUnknown**)&xrEv_[i]);
                poolClean = poolClean && xrSrv_[i][0] == nullptr && xrSrv_[i][1] == nullptr &&
                            xrTex_[i][0] == nullptr && xrTex_[i][1] == nullptr &&
                            sharedSurf9_[i][0] == nullptr && sharedSurf9_[i][1] == nullptr &&
                            shared9_[i][0] == nullptr && shared9_[i][1] == nullptr &&
                            srcShared11_[i][0] == nullptr && srcShared11_[i][1] == nullptr &&
                            xrEv_[i] == nullptr;
            }
            SafeRelease((IUnknown**)&d9Ev_);
#if defined(VRSIM_QA_D9_BEGIN_POLL) && defined(VRSIM_QA_D9_GPU_TIMING)
            // Observer finalize ONLY inside the ORIGINAL proven SourceUI
            // ownership scope: after the original all-slots-FREE drainage
            // proof, before g1::Cleanup(ctx_) releases the source device refs,
            // inside the original FPU scope. No observer Issue/GetData/wait
            // happens here. An unproven observer release dirties the original
            // poolClean proof, so the ORIGINAL quarantine/retain path applies.
            if (!QaObsFinalizeCacheAndRelease()) poolClean = false;
#endif
            g1::Cleanup(ctx_); // own COM/window/modules via the audited cleanup
            releasedAll = poolClean && d9Ev_ == nullptr && ctx_.releasedAll && !ctx_.unsafeRetained;
        }
        const bool fpuOk = captured && fpu.RestoreAndVerify(); // real result gates
        if (releasedAll && fpuOk) {
            // P#5: every state write here is Interlocked; finalized_ itself
            // is published by the caller under the short exclusive gate.
            InterlockedExchange(&initialized_, 0);
            InterlockedExchange(&cleanupProven_, 1);
            res = S_OK;
        } else {
            // Incomplete or unproven cleanup is never claimed as success.
            MarkQuarantined();
            res = E_FAIL;
        }
        InterlockedExchange(&sourceBusy_, 0); // our own carried flag, cleared once
        return res;
    }

    // ---- Queries -------------------------------------------------------------------
    // Called under the shared lifetime gate; the Pimpl is never freed.
    void FillStats(SharedBridgeStatsV1* out)
    {
        out->version = kStereoBridgeVersionV1;
        out->structBytes = (uint32_t)sizeof(SharedBridgeStatsV1);
        for (uint32_t i = 0; i < kStereoBridgeSlotCountV1; ++i)
            out->slotState[i] = (uint32_t)InterlockedCompareExchange(&state_[i], 0, 0);
        out->initCount = (uint64_t)InterlockedCompareExchange64(&cInit_, 0, 0);
        out->prepareCount = (uint64_t)InterlockedCompareExchange64(&cPrepare_, 0, 0);
        out->commitCount = (uint64_t)InterlockedCompareExchange64(&cCommit_, 0, 0);
        out->dropCount = (uint64_t)InterlockedCompareExchange64(&cDrop_, 0, 0);
        out->drainCount = (uint64_t)InterlockedCompareExchange64(&cDrain_, 0, 0);
        out->quarantineCount = (uint64_t)InterlockedCompareExchange64(&cQuarantine_, 0, 0);
    }

#ifdef VRSIM_QA_D9_BEGIN_POLL
    // =========================================================================
    // INTERNAL QA Begin/Poll (VRSIM_QA_D9_BEGIN_POLL only). One preallocated
    // ledger inside the Pimpl owns ONE persistent D9 pair: directly acquired
    // g1::Ctx device/parent refs + two g2::Eye source refs (never shallow
    // copies of borrowed numbers), fixed Init epoch/generation, monotonic
    // nonzero nonce, immutable source stamp numbers (preservation proof only),
    // cumulative 128-real-GetData/1000ms budgets, cancel and quarantine
    // states. Mutable COM bookkeeping is source-thread only; cross-thread
    // visibility is via the short shared lifetime gate + Interlocked numeric
    // mirrors (FillQaSnapshot) -- no COM work under any lock. The FPU scope is
    // per call and restore-verified even on pending returns; no FPU/SRW/
    // EntryGuard object is ever preserved across callbacks.
    // =========================================================================

    bool QaLedgerOccupied()
    {
        return InterlockedCompareExchange(&qaLedgerState_, 0, 0) != 0;
    }

    // Idle/source-thread-only synthetic fault plan setter (QA branch
    // selection only; real calls and results stay recorded separately).
    HRESULT QaSetFaultPlan(uint32_t mask)
    {
        const LONG src = InterlockedCompareExchange(&sourceThread_, 0, 0);
        if (src == 0 || src != (LONG)GetCurrentThreadId()) return E_ACCESSDENIED;
        if (QaLedgerOccupied()) return E_ACCESSDENIED; // idle only
        InterlockedExchange(&qaFaultPlan_, (LONG)mask);
        return S_OK;
    }

    HRESULT QaRequestCancel(const QaBeginTokenV1& token, QaBeginResultV1* out)
    {
        if (out != nullptr) {
            std::memset(out, 0, sizeof(*out));
            out->version = kStereoBridgeQaStructVersionV1;
            out->structBytes = (uint32_t)sizeof(QaBeginResultV1);
            out->token = token;
        }
        if (!QaKeyMatches(token)) { // wrong key: closes nothing, marks nothing
            if (out != nullptr) {
                out->status = kQaStatusRejected;
                out->bridgeHresult = E_ACCESSDENIED;
                out->ledgerStateAfter = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
            }
            return E_ACCESSDENIED;
        }
        if (!FixOrMatchSourceThread()) {
            if (out != nullptr) {
                out->status = kQaStatusRejected;
                out->bridgeHresult = E_ACCESSDENIED;
                out->ledgerStateAfter = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
            }
            return E_ACCESSDENIED;
        }
        if (!sourceBusyAdopt()) {
            if (out != nullptr) {
                out->status = kQaStatusRejected;
                out->bridgeHresult = E_FAIL;
                out->ledgerStateAfter = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
            }
            return E_FAIL;
        }
        BusyAdopt busyGuard(&sourceBusy_); // per-call only; the lease stays as it was
        // Exact key: close admission (idempotent) and mark the ORIGINAL lease.
        // This frees and proves nothing; drain happens only via QaPoll.
        const HRESULT c = CloseAdmissionLocked();
        InterlockedExchange(&qaCancelRequested_, 1);
        InterlockedIncrement64(&qaCancel_);
        if (out != nullptr) {
            out->status = (uint32_t)InterlockedCompareExchange(&qaLastStatus_, 0, 0);
            out->bridgeHresult = c;
            out->copyExecMask = (uint32_t)InterlockedCompareExchange(&qaCopyExecMask_, 0, 0);
            out->copyOkMask = (uint32_t)InterlockedCompareExchange(&qaCopyOkMask_, 0, 0);
            out->faultConsumed = (uint32_t)InterlockedCompareExchange(&qaFaultConsumed_, 0, 0);
            out->firstFailureHr = InterlockedCompareExchange(&qaFirstFailureHr_, 0, 0);
            out->lastIssueHr = InterlockedCompareExchange(&qaLastIssueHr_, 0, 0);
            out->lastGetDataHr = InterlockedCompareExchange(&qaLastGetDataHr_, 0, 0);
            out->ledgerStateAfter = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
        }
        return c;
    }

    HRESULT QaBegin(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                    const QaSourceStampV1& stamp, QaBeginResultV1* out,
                    SharedPreparedPairV1* outTicket)
    {
        if (out == nullptr || outTicket == nullptr) return E_POINTER;
        std::memset(out, 0, sizeof(*out));
        std::memset(outTicket, 0, sizeof(*outTicket));
        out->version = kStereoBridgeQaStructVersionV1;
        out->structBytes = (uint32_t)sizeof(QaBeginResultV1);
        out->token.version = kStereoBridgeQaStructVersionV1;
        out->token.structBytes = (uint32_t)sizeof(QaBeginTokenV1);
        HRESULT res;
        try {
            res = QaBeginInner(slotIndex, leftSurface, rightSurface, stamp, out, outTicket);
        } catch (...) {
            // Unknown state: retain EVERYTHING the ledger holds, quarantine.
            QaRetainLease(9);
            out->status = kQaStatusQuarantinedRetained;
            res = E_FAIL;
        }
        InterlockedExchange(&qaLastStatus_, (LONG)out->status);
        out->bridgeHresult = (LONG)res;
        // Even early refusals must publish the TRUE post-call ledger state,
        // never a memset 0 that reads like an empty-lease proof.
        out->ledgerStateAfter = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
        return res;
    }

    HRESULT QaPoll(const QaBeginTokenV1& token, QaPollResultV1* out, SharedPreparedPairV1* outTicket)
    {
        if (out == nullptr || outTicket == nullptr) return E_POINTER;
        std::memset(out, 0, sizeof(*out));
        std::memset(outTicket, 0, sizeof(*outTicket));
        out->version = kStereoBridgeQaStructVersionV1;
        out->structBytes = (uint32_t)sizeof(QaPollResultV1);
        HRESULT res;
        try {
            res = QaPollInner(token, out, outTicket);
        } catch (...) {
            QaRetainLease(9);
            out->status = kQaStatusQuarantinedRetained;
            res = E_FAIL;
        }
        InterlockedExchange(&qaLastStatus_, (LONG)out->status);
        // Per-call evidence: lastGetDataHr keeps THIS call's real result (the
        // inner sets it ONLY when a real GetData ran; zero on refusals) and
        // ticketGranted is THIS call's grant, never the sticky lease mirror.
        out->firstFailureHr = InterlockedCompareExchange(&qaFirstFailureHr_, 0, 0);
        out->pairOkMask = (uint32_t)InterlockedCompareExchange(&qaCopyOkMask_, 0, 0);
        out->getDataCallsTotal = (uint64_t)InterlockedCompareExchange64(&qaActualGetDataCalls_, 0, 0);
        out->ticketGranted = (out->status == kQaStatusCompletedGranted) ? 1u : 0u;
        out->ledgerStateAfter = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
        out->bridgeHresult = (LONG)res;
        return res;
    }

    // Pure numeric publication (short shared gate held by the wrapper).
    void FillQaSnapshot(QaBridgeSnapshotV1* out)
    {
        out->version = kStereoBridgeQaStructVersionV1;
        out->structBytes = (uint32_t)sizeof(QaBridgeSnapshotV1);
        out->ledgerState = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
        const LONG slot = InterlockedCompareExchange(&qaLedgerSlot_, 0, 0);
        out->slotValid = slot >= 0 ? 1u : 0u;
        out->slotIndex = slot >= 0 ? (uint32_t)slot : 0u;
        out->quarantined = InterlockedCompareExchange(&quarantined_, 0, 0) != 0 ? 1u : 0u;
        out->admissionClosed = InterlockedCompareExchange(admission_, 0, 0) != 0 ? 1u : 0u;
        out->initializedNow = InterlockedCompareExchange(&initialized_, 0, 0) != 0 ? 1u : 0u;
        out->finalizing = (uint32_t)InterlockedCompareExchange(&finalizing_, 0, 0);
        out->finalized = (uint32_t)InterlockedCompareExchange(&finalized_, 0, 0);
        out->epoch = (uint64_t)InterlockedCompareExchange64(&qaLedgerEpoch_, 0, 0);
        out->generation = (uint64_t)InterlockedCompareExchange(&qaLedgerGeneration_, 0, 0);
        out->nonce = (uint64_t)InterlockedCompareExchange64(&qaLedgerNonce_, 0, 0);
        out->stampFrame = (uint64_t)InterlockedCompareExchange64(&qaStampFrame_, 0, 0);
        out->stampPose = (uint64_t)InterlockedCompareExchange64(&qaStampPose_, 0, 0);
        out->stampCaptureBeginQpc = (uint64_t)InterlockedCompareExchange64(&qaStampCaptureQpc_, 0, 0);
        out->actualCopyCalls = (uint64_t)InterlockedCompareExchange64(&qaActualCopyCalls_, 0, 0);
        out->actualIssueCalls = (uint64_t)InterlockedCompareExchange64(&qaActualIssueCalls_, 0, 0);
        out->actualGetDataCalls = (uint64_t)InterlockedCompareExchange64(&qaActualGetDataCalls_, 0, 0);
        out->firstFailureHr = InterlockedCompareExchange(&qaFirstFailureHr_, 0, 0);
        out->lastIssueHr = InterlockedCompareExchange(&qaLastIssueHr_, 0, 0);
        out->lastGetDataHr = InterlockedCompareExchange(&qaLastGetDataHr_, 0, 0);
        out->firstGetDataHr = InterlockedCompareExchange(&qaFirstGetDataHr_, 0, 0);
        out->copyExecMask = (uint32_t)InterlockedCompareExchange(&qaCopyExecMask_, 0, 0);
        out->copyOkMask = (uint32_t)InterlockedCompareExchange(&qaCopyOkMask_, 0, 0);
        out->timingOk = (uint32_t)InterlockedCompareExchange(&qaTimingOk_, 0, 0);
        out->qpcFrequency = (uint64_t)InterlockedCompareExchange64(&qaQpcFrequency_, 0, 0);
        out->startQpc = (uint64_t)InterlockedCompareExchange64(&qaStartQpc_, 0, 0);
        out->lastPollElapsedMs = (uint64_t)InterlockedCompareExchange64(&qaLastPollElapsedMs_, 0, 0);
        out->cancelRequested = (uint32_t)InterlockedCompareExchange(&qaCancelRequested_, 0, 0);
        out->ticketGranted = (uint32_t)InterlockedCompareExchange(&qaTicketGranted_, 0, 0);
        out->grantedTicket = (uint64_t)InterlockedCompareExchange64(&qaGrantedTicket_, 0, 0);
        out->queryState = (uint32_t)InterlockedCompareExchange(&qaQueryState_, 0, 0);
        out->refsHeldMask = (uint32_t)InterlockedCompareExchange(&qaRefsHeldMask_, 0, 0);
        out->quarantineCode = (uint32_t)InterlockedCompareExchange(&qaQuarantineCode_, 0, 0);
        out->faultPlan = (uint32_t)InterlockedCompareExchange(&qaFaultPlan_, 0, 0);
        out->faultConsumed = (uint32_t)InterlockedCompareExchange(&qaFaultConsumed_, 0, 0);
        out->lastStatus = (uint32_t)InterlockedCompareExchange(&qaLastStatus_, 0, 0);
        out->qaGrantCount = (uint64_t)InterlockedCompareExchange64(&qaGrant_, 0, 0);
        out->qaRejectCount = (uint64_t)InterlockedCompareExchange64(&qaReject_, 0, 0);
        out->qaPendingCount = (uint64_t)InterlockedCompareExchange64(&qaPending_, 0, 0);
        out->qaRetainedCount = (uint64_t)InterlockedCompareExchange64(&qaRetain_, 0, 0);
        out->qaCancelCount = (uint64_t)InterlockedCompareExchange64(&qaCancel_, 0, 0);
        out->sourceBusy = (uint64_t)InterlockedCompareExchange(&sourceBusy_, 0, 0);
        out->workerBusy = (uint64_t)InterlockedCompareExchange(&workerBusy_, 0, 0);
        out->activeEntries = (uint64_t)InterlockedCompareExchange(&activeEntries_, 0, 0);
        out->initCount = (uint64_t)InterlockedCompareExchange64(&cInit_, 0, 0);
        out->prepareCount = (uint64_t)InterlockedCompareExchange64(&cPrepare_, 0, 0);
        out->commitCount = (uint64_t)InterlockedCompareExchange64(&cCommit_, 0, 0);
        out->dropCount = (uint64_t)InterlockedCompareExchange64(&cDrop_, 0, 0);
        out->drainCount = (uint64_t)InterlockedCompareExchange64(&cDrain_, 0, 0);
        out->quarantineCount = (uint64_t)InterlockedCompareExchange64(&cQuarantine_, 0, 0);
        for (uint32_t i = 0; i < kStereoBridgeSlotCountV1; ++i)
            out->slotState[i] = (uint32_t)InterlockedCompareExchange(&state_[i], 0, 0);
    }

#ifdef VRSIM_QA_D9_GPU_TIMING
    // ---- observer Init/Finalize/cache surface (wrapper-visible) -------------
    // Called from InitializeInner ONLY after the original pool init succeeded,
    // inside the ORIGINAL FPU capture and the original init ownership. Real
    // creation HRESULTs decide; unsupported or partial creation releases every
    // created, NEVER-ISSUED query ref in place (inside the same proven init
    // scope) and only DISABLES observation: product initialization never fails
    // here and its behavior is unchanged.
    void QaObsInitCreate()
    {
        for (uint32_t i = 0; i < kQaD9GpuObsSlotCountV1; ++i)
        {
            QaD9ObsSlot& sl = qaObs_[i];
            sl.state = qaobs::kFree;
            sl.issueMask = 0u;
            std::memset(&sl.draft, 0, sizeof(sl.draft));
            const HRESULT h0 = ctx_.ord->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &sl.freq);
            const HRESULT h1 = ctx_.ord->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &sl.beginTs);
            const HRESULT h2 = ctx_.ord->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &sl.endTs);
            const HRESULT h3 = ctx_.ord->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &sl.disjoint);
            const bool ok = SUCCEEDED(h0) && SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3) &&
                            sl.freq != nullptr && sl.beginTs != nullptr &&
                            sl.endTs != nullptr && sl.disjoint != nullptr;
            if (!ok) {
                const bool unsupported = h0 == D3DERR_NOTAVAILABLE || h1 == D3DERR_NOTAVAILABLE ||
                                         h2 == D3DERR_NOTAVAILABLE || h3 == D3DERR_NOTAVAILABLE;
                // Created-unissued refs only; SafeRelease is null-safe for the
                // slots that never received any ref.
                for (uint32_t j = 0; j <= i; ++j) {
                    SafeRelease((IUnknown**)&qaObs_[j].freq);
                    SafeRelease((IUnknown**)&qaObs_[j].beginTs);
                    SafeRelease((IUnknown**)&qaObs_[j].endTs);
                    SafeRelease((IUnknown**)&qaObs_[j].disjoint);
                    qaObs_[j].state = qaobs::kRetired;
                }
                QaObsDisable(unsupported ? 2 : 1);
                return;
            }
        }
        LARGE_INTEGER fq;
        if (QueryPerformanceFrequency(&fq) != 0 && fq.QuadPart > 0)
            InterlockedExchange64(&qaObsQpcFreq_, (LONG64)fq.QuadPart);
        InterlockedExchange(&qaObsEnabled_, 1);
    }

    // Runs ONLY inside the ORIGINAL FinalizeResources ownership scope, after
    // the original all-slots-FREE drainage proof, BEFORE g1::Cleanup(ctx_)
    // releases the source device refs, inside the original FPU scope. Caches
    // unresolved brackets as honest INCOMPLETE (resolved=0) rows: failure is
    // unavailable, NEVER 0ms and never an inferred verdict. No observer
    // Issue/GetData/Flush/wait happens here. A surviving ref returns false so
    // the original poolClean proof fails closed into the ORIGINAL
    // quarantine/retain path (refs then kept until process exit).
    bool QaObsFinalizeCacheAndRelease()
    {
        bool clean = true;
        const uint32_t need = qaobs::kDataFreq | qaobs::kDataBegin |
                              qaobs::kDataEnd | qaobs::kDataDisjoint;
        for (uint32_t i = 0; i < kQaD9GpuObsSlotCountV1; ++i)
        {
            QaD9ObsSlot& sl = qaObs_[i];
            if (sl.state == qaobs::kPending && (sl.draft.dataMask & need) != need) {
                sl.draft.validity = 0u;
                sl.draft.resolved = 0u; // terminal caching, verdict never inferred
                InterlockedIncrement64(&qaObsIncomplete_);
                QaObsWriteRecord((LONG)i);
            }
            if (sl.state == qaobs::kPending || sl.state == qaobs::kBuilding)
                sl.state = qaobs::kRetired;
            SafeRelease((IUnknown**)&sl.freq);
            SafeRelease((IUnknown**)&sl.beginTs);
            SafeRelease((IUnknown**)&sl.endTs);
            SafeRelease((IUnknown**)&sl.disjoint);
            clean = clean && sl.freq == nullptr && sl.beginTs == nullptr &&
                    sl.endTs == nullptr && sl.disjoint == nullptr;
        }
        if (clean && qaObsRecordNext_ > 0u) { // final row published once AFTER all writes
            qaObsLastRecord_ = qaObsRecords_[qaObsRecordNext_ - 1u];
            InterlockedExchange(&qaObsLastRecordValid_, 1);
        }
        InterlockedExchange(&qaObsEnabled_, 0);
        // reason 4 ONLY when observation had NO earlier disable reason;
        // disableReason 1/2/3 are preserved through finalize.
        InterlockedCompareExchange(&qaObsDisabledReason_, 4, 0);
        if (clean) InterlockedExchange(&qaObsFinalizedCached_, 1);
        return clean;
    }

    // Pure cached mirror reads for the internal accessor: no COM, no GPU, no
    // driver query, no allocation.
    void FillQaObsCache(QaD9GpuObsCacheV1* out)
    {
        out->version = kQaD9GpuObsSchemaV1;
        out->structBytes = (uint32_t)sizeof(QaD9GpuObsCacheV1);
        out->enabled = (uint32_t)InterlockedCompareExchange(&qaObsEnabled_, 0, 0);
        out->disabledReason = (uint32_t)InterlockedCompareExchange(&qaObsDisabledReason_, 0, 0);
        out->eligibleRightCopyTxns = (uint64_t)InterlockedCompareExchange64(&qaObsEligible_, 0, 0);
        out->bracketsSampled = (uint64_t)InterlockedCompareExchange64(&qaObsSampled_, 0, 0);
        out->skippedBusy = (uint64_t)InterlockedCompareExchange64(&qaObsSkippedBusy_, 0, 0);
        out->issueFailed = (uint64_t)InterlockedCompareExchange64(&qaObsIssueFailed_, 0, 0);
        out->resolveFailed = (uint64_t)InterlockedCompareExchange64(&qaObsResolveFailed_, 0, 0);
        out->incomplete = (uint64_t)InterlockedCompareExchange64(&qaObsIncomplete_, 0, 0);
        out->validRecords = (uint64_t)InterlockedCompareExchange64(&qaObsValid_, 0, 0);
        out->overflowRecords = (uint64_t)InterlockedCompareExchange64(&qaObsOverflow_, 0, 0);
        out->resolveGetDataCalls = (uint64_t)InterlockedCompareExchange64(&qaObsGetDataCalls_, 0, 0);
        out->recordCount = (uint64_t)InterlockedCompareExchange64(&qaObsRecordCount_, 0, 0);
        out->gpuFreqTicks = (uint64_t)InterlockedCompareExchange64(&qaObsGpuFreq_, 0, 0);
        out->qpcFreqTicks = (uint64_t)InterlockedCompareExchange64(&qaObsQpcFreq_, 0, 0);
        out->deltaTicksSum = (uint64_t)InterlockedCompareExchange64(&qaObsDeltaSum_, 0, 0);
        out->deltaTicksMin = (uint64_t)InterlockedCompareExchange64(&qaObsDeltaMin_, 0, 0);
        out->deltaTicksMax = (uint64_t)InterlockedCompareExchange64(&qaObsDeltaMax_, 0, 0);
        out->finalizedCached = (uint32_t)InterlockedCompareExchange(&qaObsFinalizedCached_, 0, 0);
        out->lastRecordValid = (uint32_t)InterlockedCompareExchange(&qaObsLastRecordValid_, 0, 0);
        if (out->lastRecordValid != 0u) out->lastRecord = qaObsLastRecord_;
        else std::memset(&out->lastRecord, 0, sizeof(out->lastRecord));
    }

    // Bounded indexed copy for the internal accessor: TRUE plus ONE retained
    // numeric POD ONLY for an index below the finalizedCached-gated retained
    // count. No COM/GPU/SDK/query call, no allocation, no full-ring transfer.
    bool FillQaObsRecord(uint32_t index, QaD9GpuObsRecordV1* out)
    {
        if (InterlockedCompareExchange(&qaObsFinalizedCached_, 0, 0) == 0) return false;
        const LONG64 cnt = InterlockedCompareExchange64(&qaObsRecordCount_, 0, 0);
        if (cnt < 0) return false;
        if (index >= (uint32_t)cnt || index >= (uint32_t)kQaD9GpuObsCapacityV1) return false;
        *out = qaObsRecords_[index];
        return true;
    }
#endif // VRSIM_QA_D9_GPU_TIMING
private:
    // Exact internal lease key match (version/bytes/slot/epoch/generation/
    // nonce). An occupied ledger whose fields contradict itself is handled by
    // the quarantine paths, never by silently accepting a foreign key.
    bool QaKeyMatches(const QaBeginTokenV1& token)
    {
        if (InterlockedCompareExchange(&qaLedgerState_, 0, 0) == kQaLedgerEmpty) return false;
        if (token.version != kStereoBridgeQaStructVersionV1) return false;
        if (token.structBytes != (uint32_t)sizeof(QaBeginTokenV1)) return false;
        if (token.slotIndex >= kStereoBridgeSlotCountV1) return false;
        if (token.nonce == 0ull) return false;
        if ((LONG)token.slotIndex != InterlockedCompareExchange(&qaLedgerSlot_, 0, 0)) return false;
        if (token.epoch != (uint64_t)InterlockedCompareExchange64(&qaLedgerEpoch_, 0, 0)) return false;
        if (token.generation != (uint32_t)InterlockedCompareExchange(&qaLedgerGeneration_, 0, 0)) return false;
        if (token.nonce != (uint64_t)InterlockedCompareExchange64(&qaLedgerNonce_, 0, 0)) return false;
        return true;
    }

    // Quarantine + retain: refs and the occupied lease are NEVER freed here;
    // admission closes and finalize refuses until process exit.
    void QaRetainLease(LONG code)
    {
        InterlockedIncrement64(&qaRetain_);
        InterlockedExchange(&qaLedgerState_, kQaLedgerQuarantined);
        if (InterlockedCompareExchange(&qaQueryState_, 0, 0) != kQaQueryCompleteProven)
            InterlockedExchange(&qaQueryState_, kQaQueryUnprovenRetained);
        if (InterlockedCompareExchange(&qaQuarantineCode_, 0, 0) == 0)
            InterlockedExchange(&qaQuarantineCode_, code);
        MarkQuarantined();
    }

    // Overflow-safe elapsed against the lease's ORIGINAL fixed positive QPC
    // frequency/start, with a monotone observation check. failCode: 5 = timer
    // missing/non-monotone/untrustworthy; 4 = real elapsed reached the lease's
    // cumulative 1000ms budget. This never claims hard wall-time containment
    // (an in-process driver call cannot be preempted); it only decides whether
    // a return is still allowed to grant or must retain.
    bool QaElapsedProven(uint64_t& elapsedMsOut, LONG& failCode)
    {
        elapsedMsOut = 0;
        if (InterlockedCompareExchange(&qaTimingOk_, 0, 0) == 0) { failCode = 5; return false; }
        LARGE_INTEGER now;
        if (QueryPerformanceCounter(&now) == 0) { failCode = 5; return false; }
        if (now.QuadPart < InterlockedCompareExchange64(&qaLastQpcObserved_, 0, 0)) {
            failCode = 5; return false; // non-monotone observation: untrustworthy
        }
        InterlockedExchange64(&qaLastQpcObserved_, now.QuadPart);
        const LONG64 start = InterlockedCompareExchange64(&qaStartQpc_, 0, 0);
        const LONG64 freq = InterlockedCompareExchange64(&qaQpcFrequency_, 0, 0);
        if (freq <= 0 || freq > (LONG64)(0x7FFFFFFFFFFFFFFFll / 1000ll) || now.QuadPart < start) {
            failCode = 5; return false;
        }
        const LONG64 diff = now.QuadPart - start;
        const LONG64 secPart = diff / freq;
        const LONG64 rem = diff % freq; // 0 <= rem < freq, and freq*1000 cannot overflow here
        if (secPart > (LONG64)(0x7FFFFFFFFFFFFFFFll / 1000ll)) {
            elapsedMsOut = 0xFFFFFFFFFFFFFFFFull; // saturates far beyond any real budget
        } else {
            elapsedMsOut = (uint64_t)secPart * 1000ull + (uint64_t)((rem * 1000) / freq);
        }
        InterlockedExchange64(&qaLastPollElapsedMs_,
                              elapsedMsOut > (uint64_t)0x7FFFFFFFFFFFFFFFull ? (LONG64)0x7FFFFFFFFFFFFFFFull
                                                                             : (LONG64)elapsedMsOut);
        if (elapsedMsOut >= (uint64_t)g1::DEADLINE_MS) { failCode = 4; return false; }
        return true;
    }

    void QaReleaseQaClaim(LONG slot)
    {
        InterlockedCompareExchange(&state_[slot], kFree, kPreparing);
        InterlockedExchange(&qaLedgerState_, kQaLedgerEmpty);
        InterlockedExchange(&qaLedgerSlot_, -1);
        InterlockedExchange64(&qaLedgerNonce_, 0);
        InterlockedExchange64(&qaLedgerEpoch_, 0);
        InterlockedExchange(&qaLedgerGeneration_, 0);
        InterlockedExchange64(&qaStampFrame_, 0);
        InterlockedExchange64(&qaStampPose_, 0);
        InterlockedExchange64(&qaStampCaptureQpc_, 0);
    }

    // Release every ledger-owned ref exactly once (caller holds the per-call
    // FPU capture and the source thread). Proven = released flags + audited
    // cleanup for the scratch device/parent refs.
    bool QaReleaseOwned()
    {
        g2::ReleaseEye(qaLedL_);
        g2::ReleaseEye(qaLedR_);
        g1::Cleanup(qaLedCtx_);
        const bool clean = qaLedL_.released && qaLedR_.released &&
                           qaLedCtx_.releasedAll && !qaLedCtx_.unsafeRetained;
        if (clean) InterlockedExchange(&qaRefsHeldMask_, 0);
        return clean;
    }

    // Copies + ONE Issue + at most ONE GetData. Returns:
    //  1 immediate real S_OK with both copies OK (grant path eligible)
    //  0 immediate real S_FALSE pending (retain, token valid)
    // -1 pair failure whose submitted work drained (no ticket ever)
    // -2 quarantined + retained (already marked)
    // -3 pre-work failure, nothing submitted (caller rolls back proven-clean)
    LONG QaSubmitDrain(LONG slot, QaBeginResultV1* out)
    {
        const LONG plan = InterlockedCompareExchange(&qaFaultPlan_, 0, 0);
        LONG consumed = 0;

        // (1) Left StretchRect. A failed/skipped call here submitted NOTHING
        //     (Gate2 precedent): clean pre-work rollback.
        bool leftOk = false;
        if ((plan & (LONG)kQaFaultSkipLeftCopy) != 0) {
            consumed |= (LONG)kQaFaultSkipLeftCopy;
            InterlockedExchange(&qaFaultConsumed_, consumed);
            return -3; // no real copy call executed; firstFailure stays none-real
        } else {
            const HRESULT hr = ctx_.ord->StretchRect(qaLedL_.surf, nullptr, sharedSurf9_[slot][0],
                                                     nullptr, D3DTEXF_NONE);
            InterlockedIncrement64(&qaActualCopyCalls_);
            InterlockedOr(&qaCopyExecMask_, 1);
            if (SUCCEEDED(hr)) { InterlockedOr(&qaCopyOkMask_, 1); leftOk = true; }
            else InterlockedCompareExchange(&qaFirstFailureHr_, (LONG)hr, 0);
        }
        if (!leftOk) return -3;

        // (2) Right StretchRect. From here real work IS submitted: every later
        //     uncertainty is unproven work and the pair can never be granted.
        if ((plan & (LONG)kQaFaultSkipRightCopy) != 0) {
            consumed |= (LONG)kQaFaultSkipRightCopy;
            InterlockedExchange(&qaFaultConsumed_, consumed);
        } else {
#ifdef VRSIM_QA_D9_GPU_TIMING
            // Bracket ONLY the real right copy (left already succeeded; the
            // skip-left return above and the skip-right synthetic branch both
            // issue zero observer GPU commands). Busy/no-sample -> -1: never a
            // wait, never a reissue of an occupied or unproven slot.
            const int qaObsSlot = QaObsRightBegin(slot);
#endif
            const HRESULT hr = ctx_.ord->StretchRect(qaLedR_.surf, nullptr, sharedSurf9_[slot][1],
                                                     nullptr, D3DTEXF_NONE);
#ifdef VRSIM_QA_D9_GPU_TIMING
            // Close the observer bracket BEFORE the original persistent EVENT
            // Issue in step (3), including the right-copy FAILURE case: every
            // real observer command precedes the original Issue(D3DISSUE_END).
            if (qaObsSlot >= 0) QaObsRightClose(qaObsSlot, hr);
#endif
            InterlockedIncrement64(&qaActualCopyCalls_);
            InterlockedOr(&qaCopyExecMask_, 2);
            if (SUCCEEDED(hr)) InterlockedOr(&qaCopyOkMask_, 2);
            else InterlockedCompareExchange(&qaFirstFailureHr_, (LONG)hr, 0);
        }

        // (3) ONE Issue on the single persistent EVENT (also covers the left
        //     when the right failed/skipped after the left was submitted).
        bool issueOk = false;
        if ((plan & (LONG)kQaFaultSkipIssueAfterSubmit) != 0) {
            consumed |= (LONG)kQaFaultSkipIssueAfterSubmit;
            InterlockedExchange(&qaFaultConsumed_, consumed);
            QaRetainLease(2); // submitted work with an intentionally unissued/unproven drain
            return -2;
        }
        const LONG ir = d9Ev_->Issue(D3DISSUE_END);
        InterlockedIncrement64(&qaActualIssueCalls_);
        InterlockedExchange(&qaLastIssueHr_, (LONG)ir);
        InterlockedExchange(&qaQueryState_, kQaQueryArmedPending);
        InterlockedExchange(&qaFaultConsumed_, consumed);
        if (SUCCEEDED(ir)) issueOk = true;
        else InterlockedCompareExchange(&qaFirstFailureHr_, (LONG)ir, 0);
        if (!issueOk) { QaRetainLease(2); return -2; }

        // (4) At most ONE real GetData(FLUSH) in Begin. Synthetic skip/deadline
        //     faults NEVER fake a result: the real call is not made and the
        //     submitted work stays retained/unproven.
        if ((plan & ((LONG)kQaFaultSkipGetDataAfterSubmit | (LONG)kQaFaultForceDeadlineExceeded)) != 0) {
            consumed |= (plan & (LONG)kQaFaultSkipGetDataAfterSubmit) ? (LONG)kQaFaultSkipGetDataAfterSubmit
                                                                      : (LONG)kQaFaultForceDeadlineExceeded;
            InterlockedExchange(&qaFaultConsumed_, consumed);
            QaRetainLease((plan & (LONG)kQaFaultForceDeadlineExceeded) ? 4 : 3);
            return -2;
        }
        if (InterlockedCompareExchange(&qaTimingOk_, 0, 0) == 0) {
            QaRetainLease(5); // no trustworthy timer: a bounded pending lease cannot be proven
            return -2;
        }
        InterlockedIncrement64(&qaActualGetDataCalls_); // counted BEFORE the real call
        const LONG gd = d9Ev_->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        InterlockedExchange(&qaLastGetDataHr_, (LONG)gd);
        InterlockedExchange(&qaFirstGetDataHr_, (LONG)gd);
        out->getDataCallsThisCall = InterlockedCompareExchange64(&qaActualGetDataCalls_, 0, 0) == 1 ? 1u : 0u;
        if (gd == S_FALSE) {
            InterlockedIncrement64(&qaPending_); // the REAL observation happened, whatever follows
            // A driver call that crossed the lease deadline must not publish a
            // normal pending token: observe elapsed AFTER this real S_FALSE
            // (original start/freq kept, call counts never reset, the real
            // S_FALSE is never relabeled as S_OK). Budget/clock failure keeps
            // the lease retained with honest evidence; this is not hard
            // preemption of an in-flight call, only a grant/publish gate.
            uint64_t el = 0;
            LONG fc = 5;
            if (!QaElapsedProven(el, fc)) { QaRetainLease(fc); return -2; }
            return 0; // real pending: retain ledger + refs + PREPARING, zero ticket
        }
        if (gd == S_OK) {
            InterlockedExchange(&qaQueryState_, kQaQueryCompleteProven);
            // A long real driver call must not reach the cleanup/grant path
            // after the lease deadline: re-observe elapsed AFTER the real
            // GetData and BEFORE any grant. Exceedance retains; nothing is
            // freed, re-issued or turned into a new ticket.
            uint64_t el = 0;
            LONG fc = 5;
            if (!QaElapsedProven(el, fc)) { QaRetainLease(fc); return -2; }
            const bool pairOk = (InterlockedCompareExchange(&qaCopyOkMask_, 0, 0) & 3) == 3;
            if (!pairOk) return -1; // right failed/skipped: drained original left, NO pair ticket
            return 1;
        }
        // FAILED or any other/unknown driver result: never infer completion.
        if (FAILED(gd)) InterlockedCompareExchange(&qaFirstFailureHr_, (LONG)gd, 0);
        QaRetainLease(3);
        return -2;
    }

    // Drain of the submitted work is proven S_OK. Cleanup runs inside the
    // caller's per-call FPU capture; the REAL restore+verify result is checked
    // BEFORE any grant. An ordinary PREPARED ticket requires pair-OK, no
    // cancel, open admission re-checked under the original short grant lock
    // and a monotonic ticket; every other resolved case rolls the slot back to
    // FREE with ZERO ticket and returns non-zero.
    HRESULT QaFinishProven(LONG slot, g2::FpuPreserve& fpu, bool pairOk,
                           SharedPreparedPairV1* outTicket, bool fromBegin, uint32_t* statusOut)
    {
        const bool cancelled = InterlockedCompareExchange(&qaCancelRequested_, 0, 0) != 0;
        const bool closed = Closed();
        const bool wantGrant = pairOk && !cancelled && !closed;

        const bool clean = QaReleaseOwned();
        const bool fpuOk = fpu.RestoreAndVerify(); // restore+verify BEFORE grant; idempotent
        if (!fpuOk) {
            // An unproven restore NEVER clears the durable lease or rolls the
            // slot back, even if the release above looked clean: cleanup is
            // not proven while the FPU restore is not. Refs stay retained
            // (possibly-held mirror), the real S_OK query knowledge is
            // preserved independently, no ticket, never FREE.
            if (InterlockedCompareExchange(&qaLedgerState_, 0, 0) == kQaLedgerPreparing)
                QaRetainLease(6);
            else {
                MarkQuarantined();
                if (InterlockedCompareExchange(&qaQuarantineCode_, 0, 0) == 0)
                    InterlockedExchange(&qaQuarantineCode_, 6);
                InterlockedIncrement64(&qaRetain_);
            }
            InterlockedExchange(&qaRefsHeldMask_,
                                (LONG)(kQaRefLeftSurface | kQaRefRightSurface | kQaRefDevice |
                                       kQaRefParent)); // release outcome unproven: conservatively held
            *statusOut = kQaStatusQuarantinedRetained;
            return E_FAIL;
        }
        if (!clean) {
            QaRetainLease(7); // refs may remain: retain, never claim a cleanup
            *statusOut = kQaStatusQuarantinedRetained;
            return E_FAIL;
        }
        if (!wantGrant) {
            QaReleaseQaClaim(slot); // drain proven: cancelled/closed/failed pair -> FREE, no ticket
            *statusOut = pairOk ? kQaStatusCompletedNoGrant : kQaStatusFailedDrained;
            return E_FAIL;
        }
        HRESULT res = E_FAIL;
        bool granted = false;
        bool overflow = false;
        SharedPreparedPairV1 t{};
        AcquireSRWLockExclusive(&grantLock_);
        if (!Closed()) { // final closed check + grant are one atomic section
            uint64_t tk = 0;
            if (!NextTicketMonotonic(&nextTicket_, &tk)) {
                overflow = true;
            } else if (InterlockedCompareExchange(&state_[slot], kPrepared, kPreparing) == kPreparing) {
                InterlockedExchange64(&slotTicket_[slot], (LONG64)tk);
                InterlockedExchange(&everRead_[slot], 0);
                t.version = kStereoBridgeVersionV1;
                t.structBytes = (uint32_t)sizeof(SharedPreparedPairV1);
                t.slotIndex = (uint32_t)slot;
                t.generation = generation_;
                t.epoch = epoch_;
                t.ticket = tk;
                t.legacyLeft = legacyNum_[slot][0];
                t.legacyRight = legacyNum_[slot][1];
                t.width = width_;
                t.height = height_;
                t.format88 = kStereoBridgeFormat88V1;
                *outTicket = t;
                granted = true;
                res = S_OK;
            }
        }
        if (!granted && !overflow)
            InterlockedCompareExchange(&state_[slot], kFree, kPreparing); // closed race: proven drained, rollback
        ReleaseSRWLockExclusive(&grantLock_);
        if (overflow) {
            // Ticket space exhausted: the work is proven drained but a grant is
            // impossible; quarantine (old Prepare precedent) and never reuse.
            MarkQuarantined();
            if (InterlockedCompareExchange(&qaQuarantineCode_, 0, 0) == 0)
                InterlockedExchange(&qaQuarantineCode_, 10);
            InterlockedIncrement64(&qaRetain_);
            QaReleaseQaClaim(slot);
            *statusOut = kQaStatusQuarantinedRetained;
            return E_FAIL;
        }
        QaReleaseQaClaim(slot);
        if (granted) {
            InterlockedExchange(&qaTicketGranted_, 1); // sticky until the next claim
            InterlockedExchange64(&qaGrantedTicket_, (LONG64)outTicket->ticket);
            InterlockedIncrement64(&qaGrant_); // separate counter: cPrepare_'s meaning is unchanged
            *statusOut = fromBegin ? kQaStatusImmediateGranted : kQaStatusCompletedGranted;
            return S_OK;
        }
        InterlockedIncrement64(&qaReject_);
        *statusOut = kQaStatusCompletedNoGrant; // closed between the check and the grant
        return E_FAIL;
    }

    HRESULT QaBeginInner(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                         const QaSourceStampV1& stamp, QaBeginResultV1* out,
                         SharedPreparedPairV1* outTicket)
    {
        out->status = kQaStatusRejected;
        if (slotIndex >= kStereoBridgeSlotCountV1 || leftSurface == 0ull || rightSurface == 0ull ||
            stamp.version != kStereoBridgeQaStructVersionV1 ||
            stamp.structBytes != (uint32_t)sizeof(QaSourceStampV1)) {
            InterlockedIncrement64(&qaReject_);
            return E_INVALIDARG;
        }
        if (!LiveUsable() || Closed()) { InterlockedIncrement64(&qaReject_); return E_ACCESSDENIED; }
        if (!FixOrMatchSourceThread()) { InterlockedIncrement64(&qaReject_); return E_ACCESSDENIED; }
        // Duplicate/pending refusal BEFORE anything else: zero QI, zero copy,
        // zero Issue, zero true-lease mutation. The per-call sourceBusy_ and
        // the persistent ledger are separate states; both must be free.
        if (QaLedgerOccupied()) { InterlockedIncrement64(&qaReject_); return E_FAIL; }
        if (!sourceBusyAdopt()) { InterlockedIncrement64(&qaReject_); return E_FAIL; }
        BusyAdopt busyGuard(&sourceBusy_);

        // Atomic claim under the short grant lock: open admission + slot
        // FREE->PREPARING + ledger EMPTY->PREPARING.
        const LONG slot = (LONG)slotIndex;
        bool claimed = false;
        AcquireSRWLockExclusive(&grantLock_);
        if (!Closed() && InterlockedCompareExchange(&qaLedgerState_, kQaLedgerPreparing,
                                                    kQaLedgerEmpty) == kQaLedgerEmpty) {
            if (InterlockedCompareExchange(&state_[slot], kPreparing, kFree) == kFree)
                claimed = true;
            else
                InterlockedExchange(&qaLedgerState_, kQaLedgerEmpty);
        }
        ReleaseSRWLockExclusive(&grantLock_);
        if (!claimed) { InterlockedIncrement64(&qaReject_); return E_FAIL; }

        const LONG64 nonce = InterlockedIncrement64(&qaNonceSeq_);
        if (nonce <= 0) { // exhausted counter: refuse, nothing acquired yet
            QaReleaseQaClaim(slot);
            InterlockedIncrement64(&qaReject_);
            return E_FAIL;
        }

        // Identity + stamp fixed at claim; immutable while the lease is occupied.
        InterlockedExchange(&qaLedgerSlot_, slot);
        InterlockedExchange64(&qaLedgerEpoch_, (LONG64)epoch_);
        InterlockedExchange(&qaLedgerGeneration_, (LONG)generation_);
        InterlockedExchange64(&qaLedgerNonce_, nonce);
        InterlockedExchange64(&qaStampFrame_, (LONG64)stamp.frameSerial);
        InterlockedExchange64(&qaStampPose_, (LONG64)stamp.poseSerial);
        InterlockedExchange64(&qaStampCaptureQpc_, (LONG64)stamp.captureBeginQpc);
        InterlockedExchange(&qaCopyExecMask_, 0);
        InterlockedExchange(&qaCopyOkMask_, 0);
        InterlockedExchange(&qaFirstFailureHr_, 0);
        InterlockedExchange(&qaLastIssueHr_, 0);
        InterlockedExchange(&qaLastGetDataHr_, 0);
        InterlockedExchange(&qaFirstGetDataHr_, 0);
        InterlockedExchange64(&qaActualCopyCalls_, 0);
        InterlockedExchange64(&qaActualIssueCalls_, 0);
        InterlockedExchange64(&qaActualGetDataCalls_, 0);
        InterlockedExchange(&qaCancelRequested_, 0);
        InterlockedExchange(&qaTicketGranted_, 0);
        InterlockedExchange64(&qaGrantedTicket_, 0);
        InterlockedExchange(&qaTimingOk_, 0);
        InterlockedExchange64(&qaStartQpc_, 0);
        InterlockedExchange64(&qaQpcFrequency_, 0);
        InterlockedExchange64(&qaLastPollElapsedMs_, 0);
        InterlockedExchange64(&qaLastQpcObserved_, 0);
        InterlockedExchange(&qaRefsHeldMask_, 0);
        InterlockedExchange(&qaQueryState_, kQaQueryNone);
        InterlockedExchange(&qaQuarantineCode_, 0);
        InterlockedExchange(&qaFaultConsumed_, 0);
        qaLedL_.released = false; // source-thread-only bookkeeping (members persist across leases)
        qaLedR_.released = false;

        out->token.slotIndex = slotIndex;
        out->token.epoch = epoch_;
        out->token.generation = generation_;
        out->token.nonce = (uint64_t)nonce;

        g2::FpuPreserve fpu; // per-call capture; NEVER preserved across callbacks
        fpu.Capture();
        if (!fpu.Captured()) {
            // No COM/D3D9 call was made yet: roll back the claim; an unproven
            // capture quarantines exactly like the original Prepare (P#4).
            QaReleaseQaClaim(slot);
            MarkQuarantined();
            InterlockedExchange(&qaQuarantineCode_, 1);
            out->status = kQaStatusPreWorkRolledBack;
            return E_FAIL;
        }

        // Own refs acquired DIRECTLY into the ledger while the borrowed numbers
        // are still valid, under the original AcquireSources + canonical/desc/
        // parent gates. No shallow copy of raw owning pointers anywhere.
        bool surfacesDistinct = false, sameDevice = false, beh044 = false, halType = false,
             parentOk = false, ordNotExQI = false;
        const bool acq = g2::AcquireSources(qaLedCtx_, leftSurface, rightSurface, qaLedL_, qaLedR_,
                                            surfacesDistinct, sameDevice, beh044, halType, parentOk,
                                            ordNotExQI);
        const bool gateOk = acq &&
            qaLedL_.d.Width == width_ && qaLedL_.d.Height == height_ &&
            qaLedR_.d.Width == width_ && qaLedR_.d.Height == height_ &&
            CanonicalEq((IUnknown*)qaLedCtx_.ord, (IUnknown*)ctx_.ord) &&
            CanonicalEq((IUnknown*)qaLedCtx_.d9, (IUnknown*)ctx_.d9);
        HRESULT res = E_FAIL;
        bool rollbackEligible = false; // pre-work paths: nothing was submitted
        bool rollbackClean = false;    // owned-ref release proven clean
        bool terminalRestoreVerified = false; // last fallible restore already run+checked
        if (!gateOk) {
            // Pre-submission failure: release every local QI exactly once, but
            // the claim is rolled back ONLY after the verified restore below.
            rollbackEligible = true;
            rollbackClean = QaReleaseOwned();
        } else {
            InterlockedExchange(&qaRefsHeldMask_,
                                (LONG)(kQaRefLeftSurface | kQaRefRightSurface | kQaRefDevice | kQaRefParent));

            // QPC frequency + start acquired exactly ONCE per lease; the
            // cumulative budgets are never reset by Poll/Cancel.
            LARGE_INTEGER fq, n0;
            const bool tOk = QueryPerformanceFrequency(&fq) != 0 && fq.QuadPart > 0 &&
                             QueryPerformanceCounter(&n0) != 0;
            if (tOk) {
                InterlockedExchange64(&qaQpcFrequency_, (LONG64)fq.QuadPart);
                InterlockedExchange64(&qaStartQpc_, (LONG64)n0.QuadPart);
                InterlockedExchange64(&qaLastQpcObserved_, (LONG64)n0.QuadPart);
                InterlockedExchange(&qaTimingOk_, 1);
            }
#ifdef VRSIM_QA_D9_GPU_TIMING
            // Authorized SourceUI Begin entry with the per-call FPU capture
            // held: resolve at most ONE previously-issued observer field
            // (<=1 observer GetData TOTAL across both slots per entry) before
            // this transaction's own GPU work. Original sequence unchanged.
            QaObsResolveOne();
#endif

            const LONG outcome = QaSubmitDrain(slot, out);
            if (outcome == 1 || outcome == -1) {
                // Refusal faults must not depend on the work happening to
                // reach a later Poll: consume 32/64 here too, ONLY after the
                // real immediate GetData S_OK observation, before cleanup or
                // grant. Real call counters/HRESULTs/query proof stay exactly
                // as observed; the lease is retained occupied-terminal with no
                // cleanup, no FREE, no reuse of old RGB and no ticket.
                const LONG plan = InterlockedCompareExchange(&qaFaultPlan_, 0, 0);
                const uint32_t refuse =
                    (plan & (LONG)kQaFaultRetainInsteadOfGrant) != 0u
                        ? (uint32_t)kQaFaultRetainInsteadOfGrant
                        : ((plan & (LONG)kQaFaultSimulateFpuRestoreUnproven) != 0u
                               ? (uint32_t)kQaFaultSimulateFpuRestoreUnproven
                               : 0u);
                if (refuse != 0u) {
                    InterlockedOr(&qaFaultConsumed_, (LONG)refuse);
                    const bool realRestore = fpu.RestoreAndVerify(); // REAL restore run+checked
                    if (InterlockedCompareExchange(&qaLedgerState_, 0, 0) == kQaLedgerPreparing)
                        QaRetainLease(realRestore ? 11 : 6);
                    InterlockedExchange(&qaRefsHeldMask_,
                                        (LONG)(kQaRefLeftSurface | kQaRefRightSurface | kQaRefDevice |
                                               kQaRefParent)); // refs intentionally retained
                    out->status = kQaStatusQuarantinedRetained;
                    terminalRestoreVerified = true; // above restore is final; nothing fallible follows
                } else {
                    const bool pairOk = outcome == 1;
                    // QaFinishProven ran cleanup + the REAL restore+verify BEFORE
                    // any grant or claim resolution; every later step inside it is
                    // interlocked-only. The pinned helper re-executes fesetenv and
                    // readback on EVERY call, so it must NOT be re-run after a
                    // published ticket or resolved lease (Repair1 R1).
                    res = QaFinishProven(slot, fpu, pairOk, outTicket, true, &out->status);
                    terminalRestoreVerified = true;
                }
            } else if (outcome == 0) {
                out->status = kQaStatusPending;
                res = S_FALSE; // owned refs + PREPARING retained; token valid; zero ticket
            } else if (outcome == -3) {
                rollbackEligible = true; // nothing was submitted
                rollbackClean = QaReleaseOwned();
            } else { // -2: already retained/quarantined inside QaSubmitDrain
                out->status = kQaStatusQuarantinedRetained;
            }
        }

        // VERIFIED-RESTORE TAIL: every post-capture path gets its actual
        // restore+verification checked BEFORE any ticket publication or lease
        // resolution (gate-failure rollback, -3 rollback and pending land
        // here; the refusal and QaFinishProven paths above already ran and
        // checked the last fallible restore with only interlocked steps
        // after it, so no re-verification happens after publication). No
        // COM/D3D call runs after this point on any returning path.
        if (!terminalRestoreVerified) {
            const bool fpuOk = fpu.RestoreAndVerify(); // the last fallible check on this path
            if (!fpuOk) {
                // Unproven restore: keep the durable lease occupied; never roll
                // back to FREE, never leave admission open, and preserve the real
                // S_OK query knowledge independently.
                if (InterlockedCompareExchange(&qaLedgerState_, 0, 0) == kQaLedgerPreparing) {
                    QaRetainLease(6);
                } else {
                    MarkQuarantined();
                    if (InterlockedCompareExchange(&qaQuarantineCode_, 0, 0) == 0)
                        InterlockedExchange(&qaQuarantineCode_, 6);
                    InterlockedIncrement64(&qaRetain_);
                }
                InterlockedExchange(&qaRefsHeldMask_,
                                    (LONG)(kQaRefLeftSurface | kQaRefRightSurface | kQaRefDevice |
                                           kQaRefParent)); // release outcome unproven: conservatively held
                out->status = kQaStatusQuarantinedRetained;
                res = E_FAIL; // an unproven restore can never leave S_OK/S_FALSE standing
            } else if (rollbackEligible) {
                if (rollbackClean) {
                    QaReleaseQaClaim(slot); // proven-clean release AND verified restore
                    out->status = kQaStatusPreWorkRolledBack;
                } else {
                    QaRetainLease(8); // release unproven: retain the lease, never claim a clean rollback
                    out->status = kQaStatusQuarantinedRetained;
                }
                res = E_FAIL;
            }
        }

        out->copyExecMask = (uint32_t)InterlockedCompareExchange(&qaCopyExecMask_, 0, 0);
        out->copyOkMask = (uint32_t)InterlockedCompareExchange(&qaCopyOkMask_, 0, 0);
        out->faultConsumed = (uint32_t)InterlockedCompareExchange(&qaFaultConsumed_, 0, 0);
        out->firstFailureHr = InterlockedCompareExchange(&qaFirstFailureHr_, 0, 0);
        out->lastIssueHr = InterlockedCompareExchange(&qaLastIssueHr_, 0, 0);
        out->lastGetDataHr = InterlockedCompareExchange(&qaLastGetDataHr_, 0, 0);
        out->ledgerStateAfter = (uint32_t)InterlockedCompareExchange(&qaLedgerState_, 0, 0);
        if (out->status != kQaStatusPending) {
            // A token is published ONLY with a real pending S_FALSE lease.
            std::memset(&out->token, 0, sizeof(out->token));
            out->token.version = kStereoBridgeQaStructVersionV1;
            out->token.structBytes = (uint32_t)sizeof(QaBeginTokenV1);
        }
        return res;
    }

    HRESULT QaPollInner(const QaBeginTokenV1& token, QaPollResultV1* out,
                        SharedPreparedPairV1* outTicket)
    {
        out->status = kQaStatusRejected;
        const LONG st = InterlockedCompareExchange(&qaLedgerState_, 0, 0);
        if (st == kQaLedgerEmpty) { InterlockedIncrement64(&qaReject_); return E_FAIL; }
        // Wrong key/thread: pure refusal. No GetData, no GPU work, and the
        // true lease is never mutated, cancelled or discarded by a stranger.
        if (!QaKeyMatches(token)) { InterlockedIncrement64(&qaReject_); return E_ACCESSDENIED; }
        if (!FixOrMatchSourceThread()) { InterlockedIncrement64(&qaReject_); return E_ACCESSDENIED; }
        const LONG slot = InterlockedCompareExchange(&qaLedgerSlot_, 0, 0);
        if (st == kQaLedgerQuarantined) {
            // Right key on a retained lease: honest terminal. No GPU work, no
            // freeing, no re-proving; refs stay until process exit.
            out->status = kQaStatusQuarantinedRetained;
            out->getDataCallsThisCall = 0u;
            InterlockedIncrement64(&qaReject_);
            return E_FAIL;
        }
        if (slot < 0 || (uint32_t)slot >= kStereoBridgeSlotCountV1 || !LiveUsable()) {
            // Live PREPARING lease that Init/Finalize state contradicts: an
            // INTERNAL self-contradiction is quarantine, never an external
            // wrong-key refusal.
            QaRetainLease(10);
            out->status = kQaStatusQuarantinedRetained;
            return E_FAIL;
        }
        if (!sourceBusyAdopt()) { InterlockedIncrement64(&qaReject_); return E_FAIL; }
        BusyAdopt busyGuard(&sourceBusy_);

        g2::FpuPreserve fpu; // per-call capture covering GetData + cleanup
        fpu.Capture();
        if (!fpu.Captured()) {
            QaRetainLease(1);
            out->status = kQaStatusQuarantinedRetained;
            return E_FAIL;
        }

        // Cumulative budgets across the whole lease: never reset by Poll or
        // Cancel, and a failure to read the timer cannot fake zero elapsed.
        if (InterlockedCompareExchange64(&qaActualGetDataCalls_, 0, 0) >= (LONG64)g1::MAX_POLLS) {
            QaRetainLease(4); // real call-budget exhaustion: code4, not code5
            out->status = kQaStatusQuarantinedRetained;
            if (!fpu.RestoreAndVerify()) QaRetainLease(6); // explicit; never destructor-only
            return E_FAIL;
        }
        uint64_t elapsed = 0;
        LONG failCode = 5;
        if (!QaElapsedProven(elapsed, failCode)) {
            // code4 = real elapsed reached 1000ms; code5 = QPC missing /
            // non-monotone / untrustworthy. No COM ran yet; restore explicit.
            if (failCode == 4) out->elapsedMs = elapsed;
            QaRetainLease(failCode);
            out->status = kQaStatusQuarantinedRetained;
            if (!fpu.RestoreAndVerify()) QaRetainLease(6);
            return E_FAIL;
        }
        out->elapsedMs = elapsed;
        const LONG plan = InterlockedCompareExchange(&qaFaultPlan_, 0, 0);
        // bit32/bit64 are consumed ONLY after a real S_OK below: a real
        // S_FALSE must never be turned into a claimed completed drain or a
        // synthetic retain, and no refusal may invent an HRESULT.

#ifdef VRSIM_QA_D9_GPU_TIMING
        // Observer-only Stop/cancel gate: the ORIGINAL event drain and the
        // max-poll/elapsed prechecks above run EXACTLY as before; the
        // observer resolve is placed only AFTER those prechecks pass and is
        // SKIPPED once cancellation is requested or admission is Closed
        // (Stop drain): no observer GetData after cancel/Close/Stop. Still
        // the same authorized SourceUI entry, existing sourceBusy/FPU scope,
        // <=1 observer GetData total across both slots, flags 0, no new
        // wait/Flush/Sleep/worker, lease/drain/grant logic untouched.
        if (InterlockedCompareExchange(&qaCancelRequested_, 0, 0) == 0 && !Closed())
            QaObsResolveOne();
#endif
        // Exactly ONE real GetData on this dispatch: no Sleep, no spin, no
        // pump, no render, no new pose, no re-Issue, no allocation.
        InterlockedIncrement64(&qaActualGetDataCalls_); // counted BEFORE the real call
        out->getDataCallsThisCall = 1u;
        const LONG gd = d9Ev_->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        InterlockedExchange(&qaLastGetDataHr_, (LONG)gd);
        out->lastGetDataHr = (LONG)gd;
        if (gd == S_FALSE) {
            InterlockedIncrement64(&qaPending_); // the REAL observation happened, whatever follows
            // Post-check BEFORE claiming a normal pending result: a driver
            // call that crossed the 1000ms budget (code4) or an invalid /
            // backward clock (code5) retains the original lease with the
            // untouched real S_FALSE evidence and returns non-zero. The real
            // S_FALSE is never relabeled S_OK; start/freq/call counts are not
            // reset and nothing is re-issued.
            uint64_t elP = 0;
            LONG fcP = 5;
            if (!QaElapsedProven(elP, fcP)) {
                if (fcP == 4) out->elapsedMs = elP;
                QaRetainLease(fcP);
                out->status = kQaStatusQuarantinedRetained;
                if (!fpu.RestoreAndVerify()) QaRetainLease(6); // explicit final restore+check
                return E_FAIL;
            }
            out->elapsedMs = elP;
            out->status = kQaStatusPending;
            const bool fpuOkP = fpu.RestoreAndVerify(); // valid S_FALSE restores+verifies last
            if (!fpuOkP) {
                QaRetainLease(6);
                out->status = kQaStatusQuarantinedRetained;
                return E_FAIL;
            }
            return S_FALSE;
        }
        if (gd == S_OK) {
            InterlockedExchange(&qaQueryState_, kQaQueryCompleteProven);
            // The real driver call may have crossed the lease deadline:
            // re-observe AFTER this real S_OK and BEFORE any cleanup/grant.
            uint64_t el2 = 0;
            LONG fc2 = 5;
            if (!QaElapsedProven(el2, fc2)) {
                if (fc2 == 4) out->elapsedMs = el2;
                QaRetainLease(fc2);
                out->status = kQaStatusQuarantinedRetained;
                if (!fpu.RestoreAndVerify()) QaRetainLease(6);
                return E_FAIL;
            }
            out->elapsedMs = el2;
            const uint32_t refuse =
                (plan & (LONG)kQaFaultRetainInsteadOfGrant) != 0u
                    ? (uint32_t)kQaFaultRetainInsteadOfGrant
                    : ((plan & (LONG)kQaFaultSimulateFpuRestoreUnproven) != 0u
                           ? (uint32_t)kQaFaultSimulateFpuRestoreUnproven
                           : 0u);
            if (refuse != 0u) {
                // Real S_OK observed; grant/cleanup refused: run+check the
                // REAL restore, then retain. No cleanup, no FREE, no reuse of
                // old RGB, no ticket; synthetic refusal is recorded separate
                // from the untouched real call/HRESULT/query evidence.
                InterlockedOr(&qaFaultConsumed_, (LONG)refuse);
                const bool realRestore = fpu.RestoreAndVerify();
                QaRetainLease(realRestore ? 11 : 6); // preserves the real S_OK query proof
                InterlockedExchange(&qaRefsHeldMask_,
                                    (LONG)(kQaRefLeftSurface | kQaRefRightSurface | kQaRefDevice |
                                           kQaRefParent)); // refs intentionally retained
                out->status = kQaStatusQuarantinedRetained;
                return E_FAIL;
            }
            const bool pairOk = (InterlockedCompareExchange(&qaCopyOkMask_, 0, 0) & 3) == 3;
            // QaFinishProven already ran cleanup + the REAL restore+verify
            // BEFORE grant/claim resolution; every later step inside it is
            // interlocked-only. The pinned helper re-executes fesetenv and
            // readback on EVERY call, so it is NOT run again here: no fallible
            // restore may follow a published ticket or a resolved lease
            // (Repair1 R1). Coherent per-call output: S_OK only with a new
            // granted PREPARED ticket; every non-S_OK leaves outTicket zero
            // (wrapper memset, finish writes it only on an actual grant) and
            // the wrapper derives ticketGranted from THIS call's status.
            return QaFinishProven(slot, fpu, pairOk, outTicket, false, &out->status);
        }
        // FAILED or unknown driver result: never infer completion.
        if (FAILED(gd)) InterlockedCompareExchange(&qaFirstFailureHr_, (LONG)gd, 0);
        QaRetainLease(3);
        out->status = kQaStatusQuarantinedRetained;
        if (!fpu.RestoreAndVerify()) QaRetainLease(6); // real COM ran: explicit verified restore
        return E_FAIL;
    }

#ifdef VRSIM_QA_D9_GPU_TIMING
    // ---- observer internals (source-UI-thread only while the per-call busy
    // guard + FPU capture are held) --------------------------------------------
    // Nonblocking and allocation-free: sampling, bracketing and resolving
    // never wait, never loop over data, never touch the worker; any failure
    // only makes the observation unavailable (never 0ms, never fabricated)
    // and NEVER changes the original transaction, budgets, drain, grants or
    // FPU gating around it.

    void QaObsDisable(LONG reason)
    {
        InterlockedExchange(&qaObsDisabledReason_, reason);
        InterlockedExchange(&qaObsEnabled_, 0);
    }

    // cmd: 0 freq END, 1 begin END, 2 end END, 3 disjoint BEGIN, 4 disjoint
    // END. Only REAL successful Issue sets a mask bit; the FIRST real failing
    // HRESULT is kept verbatim in the draft.
    void QaObsIssueCmd(LONG s, uint32_t cmd)
    {
        IDirect3DQuery9* q = nullptr;
        bool beginIss = false;
        uint32_t bit = 0u;
        switch (cmd) {
        case 0u: q = qaObs_[s].freq;     bit = qaobs::kIssuedFreq;          break;
        case 1u: q = qaObs_[s].beginTs;  bit = qaobs::kIssuedBegin;         break;
        case 2u: q = qaObs_[s].endTs;    bit = qaobs::kIssuedEnd;           break;
        case 3u: q = qaObs_[s].disjoint; bit = qaobs::kIssuedDisjointBegin; beginIss = true;  break;
        // cmd4 is the REAL disjoint END (bracket close): BEGIN belongs to cmd3
        // only; mask bits record the ACTUAL successful command.
        default: q = qaObs_[s].disjoint; bit = qaobs::kIssuedDisjointEnd;   beginIss = false; break;
        }
        if (q == nullptr) return; // creation never succeeded: zero observer commands
        const LONG ir = q->Issue(beginIss ? D3DISSUE_BEGIN : D3DISSUE_END);
        if (SUCCEEDED(ir)) qaObs_[s].issueMask |= bit;
        else if (qaObs_[s].draft.firstIssueHr == 0) qaObs_[s].draft.firstIssueHr = (int32_t)ir;
    }

    // Opens the bracket for the REAL right copy. Called ONLY after the left
    // copy succeeded and only when the real right StretchRect WILL execute.
    // Sampling gate: at most every 16th eligible transaction; both slots busy
    // (pending/building/retired) => skip without waiting. Returns the observer
    // slot (>=0) or -1 (do not sample). The draft carries ONLY the original
    // immutable lease stamps, the real thread id and real QPC observations;
    // the GPU fields are one source device's raw ticks, never mixed domains.
    int QaObsRightBegin(LONG leaseSlot)
    {
        if (InterlockedCompareExchange(&qaObsEnabled_, 0, 0) == 0) return -1;
        const LONG64 seq = InterlockedIncrement64(&qaObsEligible_);
        if ((seq % (LONG64)kQaD9GpuObsSampleEveryV1) != 0) return -1;
        LONG s = -1;
        for (LONG i = 0; i < (LONG)kQaD9GpuObsSlotCountV1; ++i)
            if (qaObs_[i].state == qaobs::kFree) { s = i; break; }
        if (s < 0) { InterlockedIncrement64(&qaObsSkippedBusy_); return -1; }
        QaD9ObsSlot& sl = qaObs_[s];
        sl.state = qaobs::kBuilding;
        sl.issueMask = 0u;
        std::memset(&sl.draft, 0, sizeof(sl.draft));
        QaD9GpuObsRecordV1& d = sl.draft;
        d.version = kQaD9GpuObsSchemaV1;
        d.structBytes = (uint32_t)sizeof(QaD9GpuObsRecordV1);
        d.stage = kQaD9GpuObsStageRightCopyV1; // the ONLY stage this observes
        d.obsSlot = (uint32_t)s;
        d.leaseSlot = (uint32_t)leaseSlot;
        d.sourceThreadId = (uint32_t)GetCurrentThreadId();
        d.disjointValue = -1; // unknown until a real read
        // Original lease stamps carried VERBATIM (no re-binding, no re-pose).
        d.epoch = (uint64_t)InterlockedCompareExchange64(&qaLedgerEpoch_, 0, 0);
        d.generation = (uint32_t)InterlockedCompareExchange(&qaLedgerGeneration_, 0, 0);
        d.frameSerial = (uint64_t)InterlockedCompareExchange64(&qaStampFrame_, 0, 0);
        d.poseSerial = (uint64_t)InterlockedCompareExchange64(&qaStampPose_, 0, 0);
        d.captureBeginQpc = (uint64_t)InterlockedCompareExchange64(&qaStampCaptureQpc_, 0, 0);
        LARGE_INTEGER n;
        if (QueryPerformanceCounter(&n) != 0 && n.QuadPart >= 0) {
            d.cpuBeginQpc = (uint64_t)n.QuadPart;
            d.qpcMask |= 1u;
        }
        InterlockedIncrement64(&qaObsSampled_);
        // Pre-copy observer commands, all BEFORE the original EVENT Issue:
        // freq END, disjoint BEGIN, begin TIMESTAMP END.
        QaObsIssueCmd(s, 0u);
        QaObsIssueCmd(s, 3u);
        QaObsIssueCmd(s, 1u);
        return (int)s;
    }

    // Closes the bracket right after the real right copy returned (also when
    // it FAILED): end TIMESTAMP END, then disjoint END only if its BEGIN
    // really issued (always try to close an opened disjoint bracket before
    // the original EVENT). Stamps the real right-copy HRESULT and the
    // observation-ready QPC (NOT a GPU completion time). Partial or zero
    // Issue: honest unavailable row, issueFailed++, the slot is RETIRED and
    // never reissued/reused; the refs still follow the ORIGINAL finalize/
    // retention path. Fully issued: slot PENDING, resolved later field by
    // field on authorized SourceUI entries.
    void QaObsRightClose(int sIdx, HRESULT rightCopyHr)
    {
        const LONG s = (LONG)sIdx;
        QaD9ObsSlot& sl = qaObs_[s];
        QaD9GpuObsRecordV1& d = sl.draft;
        d.rightCopyHr = (int32_t)rightCopyHr;
        d.rightCopyHrIsReal = 1u;
        QaObsIssueCmd(s, 2u);
        if ((sl.issueMask & qaobs::kIssuedDisjointBegin) != 0u &&
            (sl.issueMask & qaobs::kIssuedDisjointEnd) == 0u)
            QaObsIssueCmd(s, 4u);
        LARGE_INTEGER n;
        if (QueryPerformanceCounter(&n) != 0 && n.QuadPart >= 0) {
            // ACTUAL end-of-submission QPC only: readiness is never inferred
            // at submission; readyQpc/mask bit2 are stamped at the real
            // terminal of the resolution.
            d.cpuEndQpc = (uint64_t)n.QuadPart;
            d.qpcMask |= 2u;
        }
        d.issueCmdMask = sl.issueMask;
        const uint32_t full = qaobs::kIssuedFreq | qaobs::kIssuedDisjointBegin |
                              qaobs::kIssuedBegin | qaobs::kIssuedEnd | qaobs::kIssuedDisjointEnd;
        if (sl.issueMask != full) {
            InterlockedIncrement64(&qaObsIssueFailed_);
            d.validity = 0u;
            d.resolved = (sl.issueMask == 0u) ? 1u : 0u; // zero-issued: final fail row; partial: unproven
            QaObsStampReady(s); // real terminal (issue failure) reached now
            QaObsWriteRecord(s);
            sl.state = qaobs::kRetired;
            QaObsMaybeRetireDisable();
            return;
        }
        sl.state = qaobs::kPending;
    }

    // One row into the FIXED 2048 numeric POD ring: no per-frame allocation,
    // no file IO, no SDK refs. Full ring = overflow counter: the capture is
    // then explicitly INCOMPLETE, rows are never silently recycled.
    void QaObsWriteRecord(LONG s)
    {
        if (qaObsRecordNext_ >= kQaD9GpuObsCapacityV1) {
            InterlockedIncrement64(&qaObsOverflow_);
            return;
        }
        qaObsRecords_[qaObsRecordNext_] = qaObs_[s].draft;
        ++qaObsRecordNext_;
        InterlockedIncrement64(&qaObsRecordCount_);
    }

    void QaObsMaybeRetireDisable()
    {
        if (InterlockedCompareExchange(&qaObsEnabled_, 0, 0) == 0) return;
        if (qaObs_[0].state == qaobs::kRetired && qaObs_[1].state == qaobs::kRetired)
            QaObsDisable(3); // every slot retired: observation off from here
    }

    // Observation-ready QPC stamped ONLY when resolution reaches a REAL
    // terminal (all four S_OK fields, or a real failure terminal). A missing
    // QPC keeps mask bit2 absent: unavailable, never inferred.
    void QaObsStampReady(LONG s)
    {
        LARGE_INTEGER n;
        if (QueryPerformanceCounter(&n) != 0 && n.QuadPart >= 0) {
            qaObs_[s].draft.readyQpc = (uint64_t)n.QuadPart;
            qaObs_[s].draft.qpcMask |= 4u;
        }
    }

    // Final verdict of a fully resolved bracket. Validity requires ALL of:
    // real right-copy success (+hrIsReal), all five observer Issue commands
    // really succeeded with no first-failure, actual qpcFreq>0 with
    // qpcMask==7 and ordered cpuBegin<=cpuEnd<=actual ready, nonzero actual
    // GPU frequency, real disjoint FALSE, ordered same-device timestamps and
    // all four real S_OK data fields. Anything else stays unavailable (never
    // a fabricated zero duration). A valid slot may be reused (each query was
    // reset by its own successful GetData); any other verdict retires it.
    // Deltas are same-device ticks only: NEVER mixed with QPC or another
    // device's clock.
    void QaObsTryVerdict(LONG s)
    {
        QaD9ObsSlot& sl = qaObs_[s];
        QaD9GpuObsRecordV1& d = sl.draft;
        const uint32_t need = qaobs::kDataFreq | qaobs::kDataBegin |
                              qaobs::kDataEnd | qaobs::kDataDisjoint;
        if ((d.dataMask & need) != need) return; // fields still pending
        d.resolved = 1u;
        QaObsStampReady(s); // real terminal verdict reached: actual ready QPC
        const uint32_t fullIssue = qaobs::kIssuedFreq | qaobs::kIssuedDisjointBegin |
                                   qaobs::kIssuedBegin | qaobs::kIssuedEnd | qaobs::kIssuedDisjointEnd;
        const LONG64 qpf = InterlockedCompareExchange64(&qaObsQpcFreq_, 0, 0);
        const bool valid = d.disjointValue == 0 && d.gpuFreqTicks > 0ull &&
                           d.gpuEndTicks >= d.gpuBeginTicks &&
                           d.rightCopyHrIsReal == 1u && SUCCEEDED((HRESULT)d.rightCopyHr) &&
                           d.issueCmdMask == fullIssue && d.firstIssueHr == 0 &&
                           qpf > 0 && d.qpcMask == 7u &&
                           d.cpuBeginQpc <= d.cpuEndQpc && d.cpuEndQpc <= d.readyQpc;
        if (valid) {
            const uint64_t delta = d.gpuEndTicks - d.gpuBeginTicks;
            d.gpuDeltaTicks = delta;
            d.validity = 1u;
            if (!qaObsDeltaSeen_) { qaObsDeltaSeen_ = true; qaObsDeltaMinAcc_ = delta; qaObsDeltaMaxAcc_ = delta; }
            if (delta < qaObsDeltaMinAcc_) qaObsDeltaMinAcc_ = delta;
            if (delta > qaObsDeltaMaxAcc_) qaObsDeltaMaxAcc_ = delta;
            qaObsDeltaSumAcc_ += delta;
            InterlockedExchange64(&qaObsDeltaSum_, (LONG64)qaObsDeltaSumAcc_);
            InterlockedExchange64(&qaObsDeltaMin_, (LONG64)qaObsDeltaMinAcc_);
            InterlockedExchange64(&qaObsDeltaMax_, (LONG64)qaObsDeltaMaxAcc_);
            InterlockedExchange64(&qaObsGpuFreq_, (LONG64)d.gpuFreqTicks);
            InterlockedIncrement64(&qaObsValid_);
            QaObsWriteRecord(s);
            sl.state = qaobs::kFree; // fully resolved: safe reuse
        } else {
            d.validity = 0u; // unavailable, not 0ms
            InterlockedIncrement64(&qaObsIncomplete_);
            QaObsWriteRecord(s);
            sl.state = qaobs::kRetired;
            QaObsMaybeRetireDisable();
        }
    }

    // AT MOST ONE observer GetData TOTAL across both slots per authorized
    // SourceUI Begin/Poll entry, advanced on a finite (slot,field) cursor
    // {freq,begin,end,disjoint per slot} so repeated entries make staggered
    // progress without ever looping over data. Only PENDING slots and only
    // fields whose REAL Issue fully succeeded are queried; building or
    // never-issued queries are NEVER touched. Flags 0 (no Flush), correctly
    // sized UINT64/BOOL buffers, no Sleep/spin/worker. S_FALSE = real pending
    // (retain); failure/unknown = invalid+retired, never runtime-recreated.
    void QaObsResolveOne()
    {
        if (InterlockedCompareExchange(&qaObsEnabled_, 0, 0) == 0) return;
        const LONG total = (LONG)(kQaD9GpuObsSlotCountV1 * (uint32_t)qaobs::kFieldCount);
        bool anyPending = false;
        for (LONG s = 0; s < (LONG)kQaD9GpuObsSlotCountV1; ++s)
            if (qaObs_[s].state == qaobs::kPending) { anyPending = true; break; }
        if (!anyPending) { qaObsCursor_ = 0u; return; }
        const uint32_t issueBit[4] = { qaobs::kIssuedFreq, qaobs::kIssuedBegin, qaobs::kIssuedEnd,
                                       (uint32_t)(qaobs::kIssuedDisjointBegin | qaobs::kIssuedDisjointEnd) };
        const LONG start = (LONG)qaObsCursor_ % total;
        for (LONG k = 0; k < total; ++k)
        {
            const LONG p = (start + k) % total;
            const LONG s = p / (LONG)qaobs::kFieldCount;
            const uint32_t f = (uint32_t)(p % (LONG)qaobs::kFieldCount);
            QaD9ObsSlot& sl = qaObs_[s];
            if (sl.state != qaobs::kPending) continue;
            if ((sl.issueMask & issueBit[f]) != issueBit[f]) continue; // never fully issued
            if ((sl.draft.dataMask & (1u << f)) != 0u) continue;       // already resolved
            qaObsCursor_ = (uint32_t)((p + 1) % total);                // finite stagger
            uint64_t u64 = 0ull;
            BOOL disj = TRUE;
            LONG gd;
            if (f == (uint32_t)qaobs::kFieldDisjoint) gd = sl.disjoint->GetData(&disj, sizeof(disj), 0u);
            else {
                IDirect3DQuery9* q = (f == (uint32_t)qaobs::kFieldFreq) ? sl.freq :
                                     (f == (uint32_t)qaobs::kFieldBegin) ? sl.beginTs : sl.endTs;
                gd = q->GetData(&u64, sizeof(u64), 0u);
            }
            InterlockedIncrement64(&qaObsGetDataCalls_); // counted with the REAL call
            const int32_t hr = (int32_t)gd;
            if (f == (uint32_t)qaobs::kFieldFreq) sl.draft.gdHrFreq = hr;
            else if (f == (uint32_t)qaobs::kFieldBegin) sl.draft.gdHrBegin = hr;
            else if (f == (uint32_t)qaobs::kFieldEnd) sl.draft.gdHrEnd = hr;
            else sl.draft.gdHrDisjoint = hr;
            if (gd == S_OK) {
                sl.draft.dataMask |= 1u << f;
                if (f == (uint32_t)qaobs::kFieldFreq) sl.draft.gpuFreqTicks = u64;
                else if (f == (uint32_t)qaobs::kFieldBegin) sl.draft.gpuBeginTicks = u64;
                else if (f == (uint32_t)qaobs::kFieldEnd) sl.draft.gpuEndTicks = u64;
                else sl.draft.disjointValue = (disj != 0) ? 1 : 0; // Win32 BOOL: any nonzero is TRUE
                QaObsTryVerdict(s);
            } else if (gd == S_FALSE) {
                // real pending: bracket retained, resolved on later entries
            } else {
                // failed/unknown: invalid + retired, never recreated here
                InterlockedIncrement64(&qaObsResolveFailed_);
                sl.draft.validity = 0u;
                sl.draft.resolved = 1u;
                QaObsStampReady(s); // real failure terminal: actual ready moment
                QaObsWriteRecord(s);
                sl.state = qaobs::kRetired;
                QaObsMaybeRetireDisable();
            }
            return; // exactly one GetData attempt for this entry
        }
    }
#endif // VRSIM_QA_D9_GPU_TIMING
#endif // VRSIM_QA_D9_BEGIN_POLL

private:
    bool sourceBusyAdopt() { return InterlockedCompareExchange(&sourceBusy_, 1, 0) == 0; }
    bool workerBusyAdopt() { return InterlockedCompareExchange(&workerBusy_, 1, 0) == 0; }

    bool TicketIdentityMatches(const SharedPreparedPairV1& t, uint32_t& slot)
    {
        if (t.version != kStereoBridgeVersionV1 || t.structBytes != (uint32_t)sizeof(SharedPreparedPairV1))
            return false;
        if (t.slotIndex >= kStereoBridgeSlotCountV1) return false;
        if (t.ticket == 0ull) return false;
        if (t.generation != generation_ || t.epoch != epoch_) return false;
        if (t.width != width_ || t.height != height_ || t.format88 != kStereoBridgeFormat88V1) return false;
        const LONG s = (LONG)t.slotIndex;
        if (t.legacyLeft != legacyNum_[s][0] || t.legacyRight != legacyNum_[s][1]) return false;
        if ((uint64_t)InterlockedCompareExchange64(&slotTicket_[s], 0, 0) != t.ticket) return false;
        slot = t.slotIndex;
        return true;
    }

    // One D9 event, one flag-restored pool, all preallocated exactly once.
    // Runs ENTIRELY inside InitializeInner's FpuPreserve scope (captured
    // before the first SDK/COM call; restore verified after this returns).
    // Every failure from here on quarantines and retains; no GPU resource is
    // ever freed across an unproven flag restore.
    HRESULT AllocSharedPool(ID3D11Device* xrDevice, uint32_t w, uint32_t h)
    {
        const void* flagAddr = (const void*)(ctx_.ordParent + ctx_.isoff);

        HRESULT hr = ctx_.ord->CreateQuery(D3DQUERYTYPE_EVENT, &d9Ev_);
        if (FAILED(hr) || d9Ev_ == nullptr) { MarkQuarantined(); return E_FAIL; }

        for (uint32_t slot = 0; slot < kStereoBridgeSlotCountV1; ++slot) {
            for (uint32_t side = 0; side < 2u; ++side) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
                d.Format = g1::FMT88; d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                hr = ctx_.dev11->CreateTexture2D(&d, nullptr, &srcShared11_[slot][side]);
                if (FAILED(hr) || srcShared11_[slot][side] == nullptr) { MarkQuarantined(); return E_FAIL; }
                D3D11_TEXTURE2D_DESC ad{};
                srcShared11_[slot][side]->GetDesc(&ad);
                const bool descOk = ad.Width == w && ad.Height == h && ad.Format == g1::FMT88 &&
                                    ad.MipLevels == 1 && ad.ArraySize == 1 && ad.SampleDesc.Count == 1 &&
                                    ad.Usage == D3D11_USAGE_DEFAULT &&
                                    (ad.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) ==
                                        (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
                                    (ad.MiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0;
                if (!descOk) { MarkQuarantined(); return E_FAIL; }

                IDXGIResource* gi = nullptr;
                hr = srcShared11_[slot][side]->QueryInterface(__uuidof(IDXGIResource), (void**)&gi);
                if (FAILED(hr) || gi == nullptr) { MarkQuarantined(); return E_FAIL; }
                HANDLE legacy = nullptr;
                hr = gi->GetSharedHandle(&legacy); // legacy non-NT number: NEVER CloseHandle/Duplicate
                gi->Release();
                if (FAILED(hr) || legacy == nullptr) { MarkQuarantined(); return E_FAIL; }
                legacy_[slot][side] = legacy;
                legacyNum_[slot][side] = (uint64_t)(uintptr_t)legacy;

                // Exactly one D9 CreateTexture inside one scoped flag window.
                g2::FlagOut2 fo{};
                g2::FlagOpenLeaf2(ctx_.ord, (void*)flagAddr, w, h, &shared9_[slot][side], legacy, &fo);
                BOOL now = TRUE;
                const bool restoreProven = !fo.exceptionDuring && fo.originalReadOk && fo.original == FALSE &&
                                           ReadBytesSafe((void*)flagAddr, &now, sizeof(BOOL)) && now == FALSE;
                if (!restoreProven || !fo.setTrueReadback) {
                    // Unproven flag restore: never free GPU resources here.
                    MarkQuarantined();
                    return E_FAIL;
                }
                if (FAILED(fo.createRc) || shared9_[slot][side] == nullptr) { MarkQuarantined(); return E_FAIL; }
                hr = shared9_[slot][side]->GetSurfaceLevel(0, &sharedSurf9_[slot][side]);
                if (FAILED(hr) || sharedSurf9_[slot][side] == nullptr) { MarkQuarantined(); return E_FAIL; }

                hr = xrDevice->OpenSharedResource(legacy, __uuidof(ID3D11Texture2D),
                                                  (void**)&xrTex_[slot][side]);
                if (FAILED(hr) || xrTex_[slot][side] == nullptr) { MarkQuarantined(); return E_FAIL; }
                D3D11_TEXTURE2D_DESC xd{};
                xrTex_[slot][side]->GetDesc(&xd);
                if (xd.Format != g1::FMT88 || xd.Width != w || xd.Height != h || xd.MipLevels != 1 ||
                    xd.ArraySize != 1 || xd.SampleDesc.Count != 1) { MarkQuarantined(); return E_FAIL; }
                hr = xrDevice->CreateShaderResourceView(xrTex_[slot][side], nullptr, &xrSrv_[slot][side]);
                if (FAILED(hr) || xrSrv_[slot][side] == nullptr) { MarkQuarantined(); return E_FAIL; }
            }
            D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
            hr = xrDevice->CreateQuery(&qd, &xrEv_[slot]); // one owned persistent XR event per slot
            if (FAILED(hr) || xrEv_[slot] == nullptr) { MarkQuarantined(); return E_FAIL; }
        }
        return S_OK;
    }

    // Both StretchRects + ONE shared D9 completion wait, running ENTIRELY inside
    // Prepare's transaction-wide FpuPreserve scope (no per-return ignored restores).
    // Both copies are issued to the SAME device, so ONE Issue(D3DISSUE_END) on the
    // persistent d9Ev_ already covers both previous CopyResource work; one bounded
    // GETDATA_FLUSH wait (<=128 polls / 1000 ms, no message pump) drains the pair.
    // drained=true ONLY when both StretchRects succeeded AND the shared event wait
    // was proven complete. unprovenWork=true from the moment real submitted work
    // exists until a completed wait proves its drain; a failed/timed-out Issue or
    // Wait therefore keeps unprovenWork=true and Prepare quarantines while
    // retaining ALL source ownedQI / device+parent / shared target / event refs
    // (never a free-slot path). A failed StretchRect call itself submitted nothing
    // (Gate2 precedent). No readback/map/staging/encode.
    // ws (nullable): observation ONLY. When non-null it QPC-brackets the real
    // left/right copy calls, the real Issue and the SAME original wait (whose
    // internal GetData/Sleep classification/counting lives in g2::Wait9Counters);
    // copyWallTicksSum is the sum of the completed copy calls' CPU-side wall
    // windows, explicitly NOT GPU execution time. A timing failure clears only
    // the affected mask; it never flips any HRESULT, drain, grant, retain or
    // quarantine outcome here. ws==nullptr: zero added QPC/counter work.
    HRESULT CopyPairIntoSlot(LONG slot, g1::Ctx& sc, g2::Eye& L, g2::Eye& R, bool& drained,
                             bool& unprovenWork, PrepareWaitSample* ws)
    {
        if (ws != nullptr) {
            LARGE_INTEGER fq;
            if (QueryPerformanceFrequency(&fq) != 0 && fq.QuadPart > 0) {
                ws->qpcFrequency = (uint64_t)fq.QuadPart;
                ws->maskQpcFrequency = 1u;
            }
        }
        // (1) Left StretchRect failure: nothing was submitted at all — clean fail,
        //     no Issue, no Wait, drained=false, unprovenWork stays false.
        LARGE_INTEGER cA, cB;
        bool cAok = false;
        if (ws != nullptr) cAok = QueryPerformanceCounter(&cA) != 0;
        HRESULT hr = ctx_.ord->StretchRect(L.surf, nullptr, sharedSurf9_[slot][0], nullptr, D3DTEXF_NONE);
        if (ws != nullptr) {
            const bool cBok = QueryPerformanceCounter(&cB) != 0;
            ws->leftCopyCalls = 1u;
            ws->leftCopyHr = (LONG)hr;
            ws->maskLeftCopy = 1u;
            if (cAok && cBok && cB.QuadPart >= cA.QuadPart && ws->maskQpcFrequency != 0u) {
                ws->leftCopyWallTicks = (uint64_t)(cB.QuadPart - cA.QuadPart);
                ws->maskLeftWall = 1u;
            }
        }
        if (FAILED(hr)) return E_FAIL;
        // (2) Real work is submitted from here on. Mark unprovenWork BEFORE issuing
        //     the right copy so EVERY later uncertainty is honestly "unproven".
        unprovenWork = true;
        cAok = false;
        if (ws != nullptr) cAok = QueryPerformanceCounter(&cA) != 0;
        hr = ctx_.ord->StretchRect(R.surf, nullptr, sharedSurf9_[slot][1], nullptr, D3DTEXF_NONE);
        const bool rightSubmitted = SUCCEEDED(hr);
        if (ws != nullptr) {
            const bool cBok = QueryPerformanceCounter(&cB) != 0;
            ws->rightCopyCalls = 1u;
            ws->rightCopyHr = (LONG)hr;
            ws->maskRightCopy = 1u;
            if (cAok && cBok && cB.QuadPart >= cA.QuadPart && ws->maskQpcFrequency != 0u) {
                ws->rightCopyWallTicks = (uint64_t)(cB.QuadPart - cA.QuadPart);
                ws->maskRightWall = 1u;
            }
            // tD9copy: ONLY completed real calls, both walls actually measured.
            if (ws->maskLeftWall != 0u && ws->maskRightWall != 0u) {
                ws->copyWallTicksSum = ws->leftCopyWallTicks + ws->rightCopyWallTicks;
                ws->maskCopyWallSum = 1u;
            }
        }
        // One Issue + one bounded wait covers BOTH copies. A right-side StretchRect
        // failure did not submit anything for the right, but the left IS submitted,
        // so the shared Issue/Wait still runs to drain it.
        g2::Eye waitBoth("d9wait-pair");
        g2::Wait9Counters wc{};
        LARGE_INTEGER iA, iB;
        bool iAok = false;
        if (ws != nullptr) iAok = QueryPerformanceCounter(&iA) != 0;
        const LONG issueRc = d9Ev_->Issue(D3DISSUE_END);
        if (ws != nullptr) {
            const bool iBok = QueryPerformanceCounter(&iB) != 0;
            ws->issueCalls = 1u;
            ws->issueHr = (LONG)issueRc;
            ws->maskIssue = 1u;
            if (iAok && iBok && iB.QuadPart >= iA.QuadPart && ws->maskQpcFrequency != 0u) {
                ws->issueWallTicks = (uint64_t)(iB.QuadPart - iA.QuadPart);
                ws->maskIssueWall = 1u;
            }
        }
        // Fix(StageA2): maskWaitExecuted means the REAL WaitEvent9NoPump call
        // was entered. The && below short-circuits, so the wait runs only on a
        // successful Issue; a failed Issue leaves the mask 0. Pure predicate
        // re-check: no extra Issue/Wait/GetData/Sleep call is introduced.
        if (ws != nullptr && SUCCEEDED(issueRc)) ws->maskWaitExecuted = 1u;
        const bool waitDone = (SUCCEEDED(issueRc) &&
                               g2::WaitEvent9NoPump(sc, waitBoth, d9Ev_, ws != nullptr ? &wc : nullptr));
        if (ws != nullptr) {
            // Transfer ONLY what the real wait observed; empty wc (e.g. the
            // original timer-unavailable return) honestly keeps masks 0.
            ws->getDataCalls = wc.getDataCalls;
            ws->flushGetDataCalls = wc.flushGetDataCalls;
            ws->sFalseReturns = wc.sFalseReturns;
            ws->otherPendingReturns = wc.otherPendingReturns;
            ws->sleep1Calls = wc.sleep1Calls;
            ws->lastGetDataHr = wc.lastGetDataHr;
            ws->lastGetDataHrValid = wc.lastGetDataHrValid;
            ws->getDataWallTicksSum = wc.getDataWallTicksSum;
            ws->maskGetDataWall = wc.maskGetDataWall;
            ws->sleepWallTicksSum = wc.sleepWallTicksSum;
            ws->maskSleepWall = wc.maskSleepWall;
            ws->waitWallTicks = wc.waitWallTicks;
            ws->maskWaitWall = wc.maskWaitWall;
            if (ws->maskQpcFrequency == 0u && wc.qpcFrequency != 0ull) {
                ws->qpcFrequency = wc.qpcFrequency; // the wait's own successful QPF
                ws->maskQpcFrequency = 1u;
            }
        }
        if (!waitDone)
        {
            // (3) Issue failed or the bounded wait failed/timed out: submitted work
            //     exists whose drain is NOT proven. unprovenWork stays true, drained
            //     stays false; the original Prepare quarantines and retains refs.
            return E_FAIL;
        }
        // The completed shared-event wait proves the drain of everything actually
        // submitted this call.
        unprovenWork = false;
        if (!rightSubmitted)
        {
            // (4) Right copy never submitted, left drained: FAIL closed. drained
            //     MUST stay false — Prepare's ticket gate is `if (drained)` ONLY, so
            //     any drained=true here would issue a two-eye ticket from a new-left
            //     + stale-right slot. That combination is forbidden.
            return E_FAIL;
        }
        // (5) Both StretchRects succeeded and the ONE shared EVENT completion wait
        //     proved the pair drained.
        drained = true;
        return S_OK;
    }

    // ---- data ----------------------------------------------------------------
    volatile LONG* admission_;

    ID3D11Device* xrDevice_ = nullptr;       // borrowed; owner guarantees lifetime
    ID3D11DeviceContext* xrCtx_ = nullptr;   // borrowed canonical immediate context
    uint32_t width_ = 0, height_ = 0, generation_ = 0;
    uint64_t epoch_ = 0;

    volatile LONG sourceThread_ = 0; // P2: Interlocked first-claim CAS
    volatile LONG workerThread_ = 0;
    volatile LONG sourceBusy_ = 0;
    volatile LONG workerBusy_ = 0;

    g1::Ctx ctx_;                            // retained own refs: source ord device + parent + probe + D11

    ID3D11Texture2D* srcShared11_[kStereoBridgeSlotCountV1][2] = {};
    IDirect3DTexture9* shared9_[kStereoBridgeSlotCountV1][2] = {};
    IDirect3DSurface9* sharedSurf9_[kStereoBridgeSlotCountV1][2] = {};
    ID3D11Texture2D* xrTex_[kStereoBridgeSlotCountV1][2] = {};
    ID3D11ShaderResourceView* xrSrv_[kStereoBridgeSlotCountV1][2] = {};
    ID3D11Query* xrEv_[kStereoBridgeSlotCountV1] = {};
    HANDLE legacy_[kStereoBridgeSlotCountV1][2] = {};
    uint64_t legacyNum_[kStereoBridgeSlotCountV1][2] = {};
    IDirect3DQuery9* d9Ev_ = nullptr;

    volatile LONG state_[kStereoBridgeSlotCountV1] = {};
    volatile LONG64 slotTicket_[kStereoBridgeSlotCountV1] = {};
    volatile LONG everRead_[kStereoBridgeSlotCountV1] = {};
    volatile LONG64 nextTicket_ = 0;
    SRWLOCK grantLock_; // short grant lock: closed-check + ticket + state CAS

    volatile LONG quarantined_ = 0;
    volatile LONG initialized_ = 0;   // P#5: Interlocked-only access
    volatile LONG finalized_ = 0;     // P#5: Interlocked-only access
    volatile LONG finalizing_ = 0;    // set under the exclusive lifetime gate; blocks new entries
    bool gpuPhaseStarted_ = false;    // source-UI thread only: plain is fine
    volatile LONG cleanupProven_ = 0; // P#5: Interlocked-only; ALL tracked owned refs proven released (+verified FPU restore)

    volatile LONG64 cInit_ = 0, cPrepare_ = 0, cCommit_ = 0, cDrop_ = 0, cDrain_ = 0, cQuarantine_ = 0;

#ifdef VRSIM_QA_D9_BEGIN_POLL
    // ---- QA-only preallocated single-pending ledger (VRSIM_QA_D9_BEGIN_POLL) --
    // The COM pointers below are mutated/read ONLY on the fixed source UI
    // thread while the per-call sourceBusy_ guard is held; they are NEVER
    // published. Cross-thread visibility is the Interlocked numeric mirrors
    // read by FillQaSnapshot. The ledger OWNS these refs directly (no shallow
    // copy): two Eye refs + the AcquireSources scratch Ctx device/parent.
    // They are retained untouched on every quarantine path until process exit.
    g1::Ctx qaLedCtx_;
    g2::Eye qaLedL_{ "qa-ledger-left" };
    g2::Eye qaLedR_{ "qa-ledger-right" };

    volatile LONG qaLedgerState_ = 0;     // SharedBridgeQaLedgerStateV1
    volatile LONG qaLedgerSlot_ = -1;
    volatile LONG64 qaLedgerEpoch_ = 0;   // Init-fixed epoch mirror while occupied
    volatile LONG qaLedgerGeneration_ = 0;
    volatile LONG64 qaLedgerNonce_ = 0;   // 0 = none; strictly monotonic via qaNonceSeq_
    volatile LONG64 qaNonceSeq_ = 0;
    volatile LONG64 qaStampFrame_ = 0, qaStampPose_ = 0, qaStampCaptureQpc_ = 0; // immutable numbers
    volatile LONG qaCopyExecMask_ = 0, qaCopyOkMask_ = 0;
    volatile LONG qaFirstFailureHr_ = 0;  // first REAL failing HRESULT kept (0 = none)
    volatile LONG qaLastIssueHr_ = 0, qaLastGetDataHr_ = 0, qaFirstGetDataHr_ = 0;
    volatile LONG64 qaActualCopyCalls_ = 0, qaActualIssueCalls_ = 0, qaActualGetDataCalls_ = 0;
    volatile LONG qaCancelRequested_ = 0;
    volatile LONG qaTicketGranted_ = 0;   // sticky until the next claim
    volatile LONG64 qaGrantedTicket_ = 0;
    volatile LONG qaTimingOk_ = 0;
    volatile LONG64 qaStartQpc_ = 0, qaQpcFrequency_ = 0, qaLastPollElapsedMs_ = 0;
    volatile LONG64 qaLastQpcObserved_ = 0; // monotone QPC observation guard
    volatile LONG qaRefsHeldMask_ = 0;    // SharedBridgeQaRefBitsV1 mirror
    volatile LONG qaQueryState_ = 0;      // SharedBridgeQaQueryStateV1
    volatile LONG qaQuarantineCode_ = 0;
    volatile LONG qaFaultPlan_ = 0, qaFaultConsumed_ = 0;
    volatile LONG qaLastStatus_ = 0;
    volatile LONG64 qaGrant_ = 0, qaReject_ = 0, qaPending_ = 0, qaRetain_ = 0, qaCancel_ = 0;
#ifdef VRSIM_QA_D9_GPU_TIMING
    // ---- observer state (VRSIM_QA_D9_GPU_TIMING; internal QA only) --------
    // Queries/drafts/cursors/accumulators are source-UI-thread-only while the
    // per-call busy guard + FPU capture are held; every volatile LONG/LONG64
    // mirror is read/written ONLY through Interlocked. qaObsLastRecord_ is
    // published by the ORIGINAL finalize on the source thread and read by the
    // pure cached accessor after Destroy/worker join (lifecycle serialization).
    QaD9ObsSlot qaObs_[kQaD9GpuObsSlotCountV1];
    QaD9GpuObsRecordV1 qaObsRecords_[kQaD9GpuObsCapacityV1]; // fixed numeric POD ring; no allocation
    uint32_t qaObsRecordNext_ = 0;   // source-thread ring cursor
    uint32_t qaObsCursor_ = 0;       // finite (slot,field) resolve cursor: staggers entries
    uint64_t qaObsDeltaSumAcc_ = 0, qaObsDeltaMinAcc_ = 0, qaObsDeltaMaxAcc_ = 0; // source-thread accumulators
    bool qaObsDeltaSeen_ = false;
    QaD9GpuObsRecordV1 qaObsLastRecord_{};
    volatile LONG qaObsEnabled_ = 0;
    volatile LONG qaObsDisabledReason_ = 0; // 1 partial creation, 2 unsupported, 3 all slots retired, 4 finalized
    volatile LONG64 qaObsEligible_ = 0, qaObsSampled_ = 0, qaObsSkippedBusy_ = 0;
    volatile LONG64 qaObsIssueFailed_ = 0, qaObsResolveFailed_ = 0, qaObsIncomplete_ = 0;
    volatile LONG64 qaObsValid_ = 0, qaObsOverflow_ = 0, qaObsGetDataCalls_ = 0;
    volatile LONG64 qaObsRecordCount_ = 0;
    volatile LONG64 qaObsGpuFreq_ = 0, qaObsQpcFreq_ = 0;
    volatile LONG64 qaObsDeltaSum_ = 0, qaObsDeltaMin_ = -1, qaObsDeltaMax_ = 0; // min -1: none yet
    volatile LONG qaObsLastRecordValid_ = 0;
    volatile LONG qaObsFinalizedCached_ = 0;
#endif
#endif
};

// =============================================================================
// Public surface: every method takes the unified lifetime gate ONLY as a
// SHORT pure-atomic window (P#2: stable-Impl check + active-entry
// registration under shared; Init publication and the Finalize grant/final
// publish under exclusive), then runs all SDK/COM/GPU work with the gate
// RELEASED. Entry counts drop exactly once via RAII even on exception
// unwinds. All methods catch every exception (no ABI escape), fail-close on
// uncertainty and never delete the published Pimpl.
// =============================================================================
SharedStereoBridge::SharedStereoBridge() noexcept : m_impl(nullptr), m_admissionClosed(0)
{
    InitializeSRWLock(&m_gateLock);
}

SharedStereoBridge::~SharedStereoBridge()
{
    // No implicit unsafe cleanup and no free: the tracking object (and any
    // retained GPU/COM refs behind a quarantine or an unfinalized state) is
    // intentionally retained until process exit.
}

HRESULT SharedStereoBridge::InitializeOnSourceUI(ID3D11Device* xrDevice,
                                                 ID3D11DeviceContext* xrImmediateContext,
                                                 const LUID& runtimeRequiredLuid, uint32_t width,
                                                 uint32_t height, uint32_t generation, uint64_t epoch,
                                                 uint64_t borrowedLeftSurface, uint64_t borrowedRightSurface)
{
    Impl* im = nullptr;
    EntryGuard eg; // the Init itself is ONE tracked entry until it completes
    try {
        {
            // P#2: SHORT exclusive gate -- closed check, allocation, pointer
            // publication and entry registration ONLY. No SDK/COM call and
            // no GPU work happen inside the master lock.
            GateExclusive gate(&m_gateLock);
            if (m_impl != nullptr) return E_ACCESSDENIED; // one owner; a failed/closed owner is not reusable
            if (InterlockedCompareExchange(&m_admissionClosed, 0, 0) != 0) return E_ACCESSDENIED;
            im = new (std::nothrow) Impl(&m_admissionClosed);
            if (im == nullptr) return E_OUTOFMEMORY;
            m_impl = im; // published under the exclusive gate; NEVER deleted afterwards
            eg.Enter(&im->activeEntries_);
        }
        // The ENTIRE SDK/COM initialization runs with the lifetime gate
        // RELEASED; concurrent entries are refused by LiveUsable (initialized_
        // is still 0) and TryFinalize sees activeEntries_ > 0.
        try {
            return im->Initialize(xrDevice, xrImmediateContext, runtimeRequiredLuid, width, height,
                                  generation, epoch, borrowedLeftSurface, borrowedRightSurface);
        } catch (...) {
            im->QuarantineAfterThrow();
            return E_FAIL;
        }
    } catch (...) {
        if (m_impl != nullptr) m_impl->QuarantineAfterThrow();
        return E_FAIL;
    }
}

// Every GPU entry below: confirm a stable Impl and register one active entry
// under a SHORT shared gate window, release the gate, then run all D3D/GPU
// work lock-free. EntryGuard drops the count exactly once on every exit
// including exception unwinds, so TryFinalize can never be blocked by a
// leaked count. (P#2)

HRESULT SharedStereoBridge::PrepareOnSourceUI(uint32_t slotIndex, uint64_t leftSurface,
                                              uint64_t rightSurface, SharedPreparedPairV1* out,
                                              PrepareWaitSample* waitSample)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->Prepare(slotIndex, leftSurface, rightSurface, out, waitSample);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined(); // BusyAdopt + EntryGuard unwound already
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::CommitPreparedOnSourceUI(const SharedPreparedPairV1& prepared)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->Commit(prepared);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::DiscardPreparedOnSourceUI(const SharedPreparedPairV1& prepared)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->Discard(prepared);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::BeginReadOnWorker(const SharedPreparedPairV1& prepared,
                                              ID3D11ShaderResourceView** borrowedLeftSrv,
                                              ID3D11ShaderResourceView** borrowedRightSrv)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->BeginRead(prepared, borrowedLeftSrv, borrowedRightSrv);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::EndReadOnWorker(const SharedPreparedPairV1& prepared)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->EndRead(prepared);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::PollCompletedOnWorker()
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->PollCompleted();
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::CloseAdmission()
{
    // Atomic once (shared lifetime gate + grant-lock drain so the closure
    // cannot interleave between an entry's closed check and its grant CAS).
    // Prohibits new Init/Prepare/Commit/BeginRead; EndRead/Poll/Discard
    // remain available for drain; already-granted transactions finish.
    try {
        GateShared gate(&m_gateLock);
        Impl* im = m_impl;
        if (im != nullptr) return im->CloseAdmissionLocked();
        const LONG prev = InterlockedCompareExchange(&m_admissionClosed, 1, 0);
        return prev == 0 ? S_OK : S_FALSE;
    } catch (...) {
        if (m_impl != nullptr) m_impl->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::TryFinalizeOnSourceUI()
{
    Impl* im = nullptr;
    bool needsPhase2 = false;
    HRESULT g = E_FAIL;
    try {
        // Phase 1 (SHORT, pure atomics): TRY-acquire the exclusive gate so
        // brief shared holders can never queue finalize unboundedly; if the
        // gate is momentarily taken, report pending and retry later.
        GateTryExclusive gate(&m_gateLock);
        if (!gate.held) return S_FALSE;
        im = m_impl;
        if (im == nullptr) {
            // No Init ever ran: nothing owned by this bridge; finalize
            // only if the admission is actually closed.
            return InterlockedCompareExchange(&m_admissionClosed, 0, 0) != 0 ? S_OK : E_FAIL;
        }
        // Grants finalizing_ (needsPhase2=true) ONLY with 0 active entries,
        // closed admission, all slots FREE and this call's own sourceBusy_
        // legally adopted; a proven-empty failed Init instead finalizes here
        // with needsPhase2=false (P#6: no second Cleanup ever runs).
        g = im->BeginFinalizeGate(&needsPhase2);
    } catch (...) {
        if (im != nullptr) im->QuarantineAfterThrow();
        return E_FAIL;
    }
    // The gate is released here by RAII BEFORE any cleanup work. Refusals
    // and pending pass straight through (BeginFinalizeGate already released
    // its own busy on every non-grant path).
    if (g != S_OK) return g;
    if (!needsPhase2) return S_OK; // already-clean proven-empty success
    HRESULT r = E_FAIL;
    try {
        // Phase 2 (NO lock held): new entries are blocked by finalizing_,
        // in-flight entries were excluded by the 0-active-entry grant; all
        // COM Releases and the audited Cleanup run OUTSIDE any master lock,
        // under the verified FPU capture. The real HRESULT gates; anything
        // unproven quarantines inside FinalizeResources.
        r = im->FinalizeResources();
    } catch (...) {
        im->QuarantineAfterThrow();
        return E_FAIL;
    }
    if (SUCCEEDED(r)) {
        try {
            GateExclusive publish(&m_gateLock); // SHORT: final-state publication only
            im->PublishFinalized(); // finalizing_ stays 1: a finalized owner never reopens
        } catch (...) {
            im->QuarantineAfterThrow();
            return E_FAIL;
        }
    }
    return r;
}

bool SharedStereoBridge::Initialized() const noexcept
{
    try {
        GateShared gate(&m_gateLock);
        return m_impl != nullptr && m_impl->InitializedNow();
    } catch (...) {
        return false;
    }
}

bool SharedStereoBridge::Quarantined() const noexcept
{
    try {
        GateShared gate(&m_gateLock);
        return m_impl != nullptr && m_impl->Quarantined();
    } catch (...) {
        return false;
    }
}

void SharedStereoBridge::Stats(SharedBridgeStatsV1* out) const noexcept
{
    if (out == nullptr) return;
    SharedBridgeStatsV1 z{};
    z.version = kStereoBridgeVersionV1;
    z.structBytes = (uint32_t)sizeof(SharedBridgeStatsV1);
    try {
        GateShared gate(&m_gateLock);
        if (m_impl != nullptr) m_impl->FillStats(&z);
    } catch (...) {
        // keep the zeroed snapshot; never throw across the ABI
    }
    *out = z;
}

#ifdef VRSIM_QA_D9_BEGIN_POLL
// =============================================================================
// QA Begin/Poll public surface (internal C++ only, VRSIM_QA_D9_BEGIN_POLL):
// same short shared lifetime gate + EntryGuard pattern as every existing
// entry. All GPU/COM work runs with the gate released; the snapshot is pure
// Interlocked mirror reads under the short gate (no driver call, no COM, no
// allocation). Define OFF none of this exists.
// =============================================================================
HRESULT SharedStereoBridge::QaBeginOnSourceUI(uint32_t slotIndex, uint64_t leftSurface,
                                              uint64_t rightSurface, const QaSourceStampV1& stamp,
                                              QaBeginResultV1* result, SharedPreparedPairV1* outTicket)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->QaBegin(slotIndex, leftSurface, rightSurface, stamp, result, outTicket);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined(); // BusyAdopt + EntryGuard unwound already
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::QaPollOnSourceUI(const QaBeginTokenV1& token, QaPollResultV1* result,
                                             SharedPreparedPairV1* outTicket)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->QaPoll(token, result, outTicket);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::QaRequestCancelOnSourceUI(const QaBeginTokenV1& token,
                                                      QaBeginResultV1* result)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->QaRequestCancel(token, result);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

HRESULT SharedStereoBridge::QaSetFaultPlan(uint32_t faultMask)
{
    Impl* im = nullptr;
    EntryGuard eg;
    try {
        {
            GateShared gate(&m_gateLock); // SHORT: stable Impl + entry registration only
            im = m_impl;
            if (im != nullptr) eg.Enter(&im->activeEntries_);
        }
        if (im == nullptr) return E_ACCESSDENIED;
        return im->QaSetFaultPlan(faultMask);
    } catch (...) {
        if (im != nullptr) im->MarkQuarantined();
        return E_FAIL;
    }
}

void SharedStereoBridge::QaSnapshot(QaBridgeSnapshotV1* out) const noexcept
{
    if (out == nullptr) return;
    QaBridgeSnapshotV1 z{};
    z.version = kStereoBridgeQaStructVersionV1;
    z.structBytes = (uint32_t)sizeof(QaBridgeSnapshotV1);
    try {
        GateShared gate(&m_gateLock); // SHORT: pure Interlocked mirror reads only
        if (m_impl != nullptr) m_impl->FillQaSnapshot(&z);
    } catch (...) {
        // keep the zeroed snapshot; never throw across the surface
    }
    *out = z;
}
#ifdef VRSIM_QA_D9_GPU_TIMING
// Internal QA-only cached accessor: NOT an export, NOT PInvoke-targeted, NOT
// wire/ABI. Zero COM/GPU/SDK calls: pure Interlocked mirror reads under the
// SHORT shared lifetime gate (the Pimpl is never deleted, so this can never
// dangle). lastRecord is published by the ORIGINAL finalize and read only
// after Destroy/worker join (lifecycle serialization).
void SharedStereoBridge::QaD9GpuTimingCache(QaD9GpuObsCacheV1* out) const noexcept
{
    if (out == nullptr) return;
    QaD9GpuObsCacheV1 z{};
    z.version = kQaD9GpuObsSchemaV1;
    z.structBytes = (uint32_t)sizeof(QaD9GpuObsCacheV1);
    z.deltaTicksMin = 0xFFFFFFFFFFFFFFFFull; // no valid delta observed yet
    try {
        GateShared gate(&m_gateLock); // SHORT: pure Interlocked mirror reads only
        if (m_impl != nullptr) m_impl->FillQaObsCache(&z);
    } catch (...) {
        // keep the default cache; never throw across the surface
    }
    *out = z;
}

// Indexed retained-record copy: internal C++ only (no export/PInvoke/ABI).
// Returns true only for a retained index AFTER the actual finalizedCached
// flag; copies ONE numeric POD under the SHORT shared lifetime gate. Zero
// COM/GPU/SDK/query calls, zero allocation; never a hot-path full-ring copy.
bool SharedStereoBridge::QaD9GpuTimingRecord(uint32_t index, QaD9GpuObsRecordV1* out) const noexcept
{
    if (out == nullptr) return false;
    bool ok = false;
    try {
        GateShared gate(&m_gateLock); // SHORT: one bounded numeric POD copy only
        if (m_impl != nullptr) ok = m_impl->FillQaObsRecord(index, out);
    } catch (...) {
        ok = false; // never throw across the surface
    }
    return ok;
}
#endif // VRSIM_QA_D9_GPU_TIMING
#endif // VRSIM_QA_D9_BEGIN_POLL

} // namespace VRSIM
