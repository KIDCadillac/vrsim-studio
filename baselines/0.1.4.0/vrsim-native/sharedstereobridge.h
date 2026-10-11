// SharedStereoBridge.h -- CARD3A: VRSIM shared stereo bridge (x64 MSVC, C++17,
// Unicode, no C++ exceptions cross this surface).
//
// Standalone resource class: THREE fixed stereo slots, NO independent producer
// threads, NO ring/pose publishing. The managed owner calls the *OnSourceUI
// methods from the WPF render/source UI thread inside its D3DImage double-lock
// callback (borrowed IDirect3DSurface9 pointers are only valid there; native
// QIs its own short-lived refs and never stores the borrowed numbers across
// calls) and the *OnWorker methods from exactly one XR worker thread. The
// caller maps each ticket slot onto its own existing 3-slot ring.
//
// Ticket lifecycle: Prepare grants PREPARED tickets -> explicit Commit on the
// source UI thread (after managed post guards) -> worker BeginRead reserves
// the matching COMMITTED slot -> EndRead enqueues the owned persistent D11
// event -> PollCompletedOnWorker releases old tickets to FREE only on
// S_OK+TRUE. CloseAdmission is atomic-once and prohibits new Init/Prepare/
// Commit/BeginRead while EndRead/Poll/Discard stay available for drain.
// TryFinalizeOnSourceUI releases owned resources only after closed admission,
// all slots FREE and no busy guards held; unproven paths retain refs
// conservatively until process exit (fail-closed quarantine).
//
// Concurrency contract (P1 corrections, short-gate revision):
//  - One SRWLOCK lifetime gate protects the Pimpl pointer. It is held only
//    for SHORT pure-atomic windows: Init publishes under a brief exclusive
//    section; every source/worker entry confirms a stable Impl and bumps an
//    active-entry counter under a brief shared section, then RELEASES the
//    gate before any SDK/COM/GPU work and drops the counter via RAII (also
//    on exception unwinds). Cached queries take the shared gate briefly and
//    touch only atomics. Once published the Pimpl is NEVER deleted, so
//    cached Stats / Initialized / Quarantined queries can never
//    use-after-free; it is a tiny pure tracking object retained until
//    process exit by design. No COM Release, D3D call, GPU wait, Cleanup or
//    blocking driver wait happens inside the lifetime gate or grant lock.
//  - Final admission/commit/read grants and CloseAdmission are serialized on
//    one internal short grant lock: the closed check and the slot-state
//    transition (including Prepare's FREE->PREPARING start grant) happen
//    atomically together. A grant already committed may finish and drain,
//    but its final PREPARED grant is refused once closed.
//  - TryFinalize uses a TRY-acquire on the exclusive gate (never blocks
//    unboundedly behind other short holders), grants finalizing only with
//    0 active entries, closed admission and all slots FREE (the source
//    thread's own finalize busy is legal), runs the COM cleanup lock-free
//    behind the finalizing gate, and publishes the final state under a
//    short exclusive gate. Init/Finalize state flags are Interlocked so
//    lock-free publication never data-races concurrent readers.
//  - Final admission/commit/read grants and CloseAdmission are serialized on
//    one internal short grant lock: the closed check and the slot-state
//    transition happen atomically together; no D3D API or driver wait runs
//    inside that lock. A grant already committed may finish and drain.
//  - Every whole SourceUI D3D transaction (INIT and PREPARE, including
//    bootstrap identity QI calls and the final own-ref cleanup) runs under
//    the verified g2::FpuPreserve actual fenv/MXCSR capture; the REAL
//    restore-verification result decides the HRESULT. Unproven capture or
//    restore closes admission, quarantines and returns non-zero.
//  - Failed Initialize never fakes success: resources are released only on
//    a proven pre-GPU-phase cleanup (releasedAll + verified FPU restore);
//    anything unproven quarantines and retains, and TryFinalize refuses.
//
// SCOPE LIMIT: this class compiles standalone; it is NOT yet wired into the
// XR compositor and has NO producer loop attached. Nothing here claims FPS or
// XR submission -- both remain UNPROVEN until separately gated.
//
// Suggested compile (by Codex):
//   cl /std:c++17 /EHsc /DUNICODE /D_UNICODE /c SharedStereoBridge.cpp
// The .cpp reuses the Gate2-proven helpers through an ABSOLUTE-path
// #include of the pinned ObsD3D9ActualSourceQA.cpp (SHA
// FFCE39922F90D4A5760395A132D4C94E6531B63EA03B2AC1F333F02D3CC35CFF, which
// includes the audited Gate1, which includes frozen Gate0); those included
// files must not be edited. The inherited QA export ObsSourcePairGate2V1 and
// g2::Run/g2::RunEye are candidate-QA-only and are NEVER called here as a
// production loop. A later build-freeze root will redirect the include to
// pinned frozen helpers.
#ifndef VRSIM_SHARED_STEREO_BRIDGE_H
#define VRSIM_SHARED_STEREO_BRIDGE_H

#ifndef _M_X64
#error CARD3A VRSIM SharedStereoBridge requires x64
#endif

#include <windows.h>

#include <cstdint>
#include <type_traits>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace VRSIM {

// Locked first-configuration: compiled size is strictly 2048x2048 when
// VRSIM_SHARED_2048 is defined, otherwise 1536x1536; format 88
// (DXGI_FORMAT_B8G8R8X8_UNORM; the 4th byte is X, never claimed as alpha).
inline constexpr uint32_t kStereoBridgeVersionV1 = 1u;
inline constexpr uint32_t kStereoBridgeSlotCountV1 = 3u;
#ifdef VRSIM_SHARED_2048
inline constexpr uint32_t kStereoBridgeWidthV1 = 2048u;
inline constexpr uint32_t kStereoBridgeHeightV1 = 2048u;
#else
inline constexpr uint32_t kStereoBridgeWidthV1 = 1536u;
inline constexpr uint32_t kStereoBridgeHeightV1 = 1536u;
#endif
inline constexpr uint32_t kStereoBridgeFormat88V1 = 88u; // DXGI_FORMAT_B8G8R8X8_UNORM

// Slot state codes reported in SharedBridgeStatsV1::slotState:
// 0 FREE, 1 PREPARING, 2 PREPARED, 3 COMMITTED, 4 READING, 5 INFLIGHT.
enum SharedStereoSlotStateV1 : uint32_t {
    kSharedStereoFree = 0,
    kSharedStereoPreparing = 1,
    kSharedStereoPrepared = 2,
    kSharedStereoCommitted = 3,
    kSharedStereoReading = 4,
    kSharedStereoInflight = 5
};

// Fixed, trivially copyable ticket. All fields are metadata-only: the
// legacyLeft/legacyRight members are opaque historical handle NUMBERS
// (never NT handles: never CloseHandle, never DuplicateHandle) captured at
// Init while the owning resources were live.
struct SharedPreparedPairV1 {
    uint32_t version;      // kStereoBridgeVersionV1
    uint32_t structBytes;  // sizeof(SharedPreparedPairV1)
    uint32_t slotIndex;    // 0..kStereoBridgeSlotCountV1-1
    uint32_t generation;
    uint64_t epoch;        // nonzero, fixed at InitializeOnSourceUI
    uint64_t ticket;       // strict monotonic, nonzero; overflow fails closed
    uint64_t legacyLeft;
    uint64_t legacyRight;
    uint32_t width;
    uint32_t height;
    uint32_t format88;
};
static_assert(std::is_trivially_copyable<SharedPreparedPairV1>::value,
              "SharedPreparedPairV1 must stay trivially copyable");
static_assert(sizeof(SharedPreparedPairV1) == 64, "SharedPreparedPairV1 layout is fixed");

// Pure cached counters; no driver query, no allocation, safe from either
// thread (taken under the shared lifetime gate; the Pimpl is never freed).
struct SharedBridgeStatsV1 {
    uint32_t version;
    uint32_t structBytes;
    uint32_t slotState[kStereoBridgeSlotCountV1];
    uint64_t initCount;
    uint64_t prepareCount;
    uint64_t commitCount;
    uint64_t dropCount;    // discards (PREPARED, and never-read COMMITTED under closed admission)
    uint64_t drainCount;   // tickets released INFLIGHT -> FREE on proven S_OK+TRUE
    uint64_t quarantineCount;
};
static_assert(std::is_trivially_copyable<SharedBridgeStatsV1>::value,
              "SharedBridgeStatsV1 must stay trivially copyable");

class SharedStereoBridge {
public:
    // No allocation here: the Pimpl is allocated only inside a first
    // InitializeOnSourceUI call.
    SharedStereoBridge() noexcept;
    // Destructor performs NO implicit unsafe cleanup and frees NO memory:
    // the Pimpl tracking object is intentionally retained until process exit
    // on every path (failed Init, quarantine, or after a proven finalize) so
    // cached queries can never reference freed memory and retained GPU/COM
    // refs are never dropped behind an unproven state.
    ~SharedStereoBridge();
    SharedStereoBridge(const SharedStereoBridge&) = delete;
    SharedStereoBridge& operator=(const SharedStereoBridge&) = delete;

    // First valid call fixes the source UI thread; every later *OnSourceUI
    // method must run on exactly that thread. xrImmediateContext must be the
    // canonical immediate context of xrDevice. STRICT four-way same-GPU gate
    // with NO fallback: source D3D9 adapter LUID == source-side D3D11 LUID ==
    // XR device DXGI adapter LUID == runtimeRequiredLuid, plus matching
    // VendorId/DeviceId/SubSysId/Revision. width/height must be 1536/1536,
    // generation nonzero, epoch nonzero, both borrowed surface pointers
    // nonzero and live inside the caller's D3DImage double-lock callback.
    // Allocates the fixed 3x2 shared texture pool + XR SRVs + persistent
    // events exactly once under the exclusive lifetime gate; on failure
    // closes admission/quarantines unless a pre-GPU-phase cleanup was fully
    // proven (which never fakes a success HRESULT).
    HRESULT InitializeOnSourceUI(ID3D11Device* xrDevice, ID3D11DeviceContext* xrImmediateContext,
                                 const LUID& runtimeRequiredLuid, uint32_t width, uint32_t height,
                                 uint32_t generation, uint64_t epoch, uint64_t borrowedLeftSurface,
                                 uint64_t borrowedRightSurface);

    // Requires a FREE slot, open admission and stable epoch/generation (the
    // final closed check + grant happen atomically under the short grant
    // lock). QIs the borrowed surfaces (own refs only for the duration of
    // this call, never retained across the callback), proves the exact same
    // canonical actual D3D9 device/parent and configured dimensions,
    // StretchRects both sources into the owned shared D9 surfaces, and
    // proves the D9 drain with a bounded <=128-poll/1000ms GETDATA_FLUSH
    // wait (no pump). Returns the granted PREPARED ticket. The whole source
    // D3D9 transaction is wrapped in the verified FPU capture/restore. No
    // readback, map, staging or encode anywhere.
    HRESULT PrepareOnSourceUI(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                              SharedPreparedPairV1* out);

    // Exact ticket + original slot metadata + open admission -> COMMITTED,
    // granted atomically with the closed check under the short grant lock.
    HRESULT CommitPreparedOnSourceUI(const SharedPreparedPairV1& prepared);

    // PREPARED whose D9 drain is already proven -> FREE. After CloseAdmission,
    // never-read COMMITTED tickets may also be discarded for drain; READING
    // and INFLIGHT are never reclaimed here.
    HRESULT DiscardPreparedOnSourceUI(const SharedPreparedPairV1& prepared);

    // First valid worker call fixes the worker thread; it must differ from
    // the source UI thread and all later worker calls must match exactly.
    // Reserves the matching COMMITTED slot -> READING (atomically with the
    // closed check under the short grant lock) without rebinding
    // epoch/generation. The returned SRVs are BORROWED persistent pointers:
    // do not Release, do not cache past Commit/Read pairing lifetime.
    HRESULT BeginReadOnWorker(const SharedPreparedPairV1& prepared,
                              ID3D11ShaderResourceView** borrowedLeftSrv,
                              ID3D11ShaderResourceView** borrowedRightSrv);

    // READING (matching ticket) -> enqueues the owned persistent D11 event,
    // Flush, state INFLIGHT. Safe even if the consumer read nothing after a
    // successful BeginRead. No synchronous readback here.
    HRESULT EndReadOnWorker(const SharedPreparedPairV1& prepared);

    // For each INFLIGHT slot: GetData with the actual 4-byte Win32 BOOL;
    // ONLY S_OK + TRUE releases that old ticket INFLIGHT -> FREE. S_FALSE is
    // pending. Any other HRESULT quarantines and retains. No sleeps, no
    // thread pump, no allocation on any path.
    HRESULT PollCompletedOnWorker();

    // Atomic once (shared lifetime gate + short grant lock so the closure
    // cannot interleave between an entry's closed check and its grant CAS).
    // Prohibits new Init/Prepare/Commit/BeginRead; EndRead, Poll and Discard
    // remain available for drain; already-granted transactions finish. S_OK
    // first time, S_FALSE after that (including after quarantine).
    HRESULT CloseAdmission();

    // Requires closed admission, no quarantine, all slots FREE and no busy
    // guard held by another call. S_FALSE while still pending (slots
    // draining or a worker call in flight). Phase 1 runs under the exclusive
    // lifetime gate (proven-empty failed Init may finalize there); phase 2
    // releases owned resources (pool, SRVs, events, own probe/window/modules
    // via the audited cleanup) OUTSIDE the lock but behind the finalizing
    // gate that blocks every new entry, under the verified FPU capture. S_OK
    // only when ALL tracked owned resources are proven released and both GPU
    // queues were proven drained; anything unproven quarantines/retains with
    // a non-zero result. After a proven finalize the closed owner is not
    // reusable.
    HRESULT TryFinalizeOnSourceUI();

    // Pure cached queries; callable from either thread (shared lifetime gate;
    // the Pimpl is never freed, so no UAF and no driver call).
    bool Initialized() const noexcept;
    bool Quarantined() const noexcept;
    void Stats(SharedBridgeStatsV1* out) const noexcept;

private:
    class Impl;
    // Allocated and published ONLY by InitializeOnSourceUI under the
    // exclusive lifetime gate; NEVER deleted on any path (retained tracking
    // object until process exit). Reads are gated by m_gateLock.
    Impl* m_impl;
    // Master admission flag; lives for the whole object lifetime even before
    // the Pimpl exists, so CloseAdmission can prohibit a later first Init.
    volatile LONG m_admissionClosed;
    // Unified lifetime/state gate: shared for every entry and cached query,
    // exclusive for Init publication and the Finalize grant/final-state
    // publication. It is held ONLY for short pure-atomic windows (pointer
    // check, entry registration, state publish) -- never across SDK/COM,
    // GPU work, or waits. mutable: it is a logical-const synchronization
    // object read by the const cached queries.
    mutable SRWLOCK m_gateLock;
};

} // namespace VRSIM

#endif // VRSIM_SHARED_STEREO_BRIDGE_H
