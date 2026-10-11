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
    HRESULT Prepare(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                    SharedPreparedPairV1* out)
    {
        if (out == nullptr) return E_POINTER;
        if (slotIndex >= kStereoBridgeSlotCountV1) return E_INVALIDARG;
        if (leftSurface == 0ull || rightSurface == 0ull) return E_INVALIDARG;
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
                    res = CopyPairIntoSlot(slot, sc, L, R, drained, unprovenWork);
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

    // Both StretchRects + bounded D9 drain, running ENTIRELY inside Prepare's
    // transaction-wide FpuPreserve scope (no per-return ignored restores).
    // drained=true only when BOTH submissions' ISSUE_END + <=128-poll/1000ms
    // GETDATA_FLUSH waits (no pump) were proven; unprovenWork=true whenever
    // real submissions exist whose drain is not proven -- Prepare then
    // quarantines and retains ALL refs. A failed StretchRect call itself
    // submitted nothing (Gate2 precedent). No readback/map/staging/encode.
    HRESULT CopyPairIntoSlot(LONG slot, g1::Ctx& sc, g2::Eye& L, g2::Eye& R, bool& drained,
                             bool& unprovenWork)
    {
        HRESULT hr = ctx_.ord->StretchRect(L.surf, nullptr, sharedSurf9_[slot][0], nullptr, D3DTEXF_NONE);
        if (FAILED(hr)) return E_FAIL; // nothing submitted; clean fail
        g2::Eye waitL("d9wait-l");
        hr = d9Ev_->Issue(D3DISSUE_END);
        if (FAILED(hr) || !g2::WaitEvent9NoPump(sc, waitL, d9Ev_)) {
            unprovenWork = true;
            return E_FAIL;
        }
        hr = ctx_.ord->StretchRect(R.surf, nullptr, sharedSurf9_[slot][1], nullptr, D3DTEXF_NONE);
        if (FAILED(hr)) return E_FAIL; // left drained already; right never submitted
        g2::Eye waitR("d9wait-r");
        hr = d9Ev_->Issue(D3DISSUE_END);
        if (FAILED(hr) || !g2::WaitEvent9NoPump(sc, waitR, d9Ev_)) {
            unprovenWork = true;
            return E_FAIL;
        }
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
                                              uint64_t rightSurface, SharedPreparedPairV1* out)
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
        return im->Prepare(slotIndex, leftSurface, rightSurface, out);
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

} // namespace VRSIM
