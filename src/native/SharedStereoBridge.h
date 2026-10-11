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

// Locked first-configuration: compiled size is strictly 3072x3072 when
// VRSIM_SHARED_3072 is defined, strictly 2736x2736 when VRSIM_SHARED_2736 is
// defined, strictly 2560x2560 when VRSIM_SHARED_2560 is defined, strictly
// 2048x2048 when VRSIM_SHARED_2048 is defined, otherwise 1536x1536; format 88
// (DXGI_FORMAT_B8G8R8X8_UNORM; the 4th byte is X, never claimed as alpha).
// VRSIM_SHARED_3072, VRSIM_SHARED_2736, VRSIM_SHARED_2560 and
// VRSIM_SHARED_2048 are mutually exclusive compile variants: defining any two
// together is a hard compile-time error.
#if defined(VRSIM_SHARED_3072) && defined(VRSIM_SHARED_2736)
#error VRSIM_SHARED_3072 and VRSIM_SHARED_2736 are mutually exclusive; define at most one
#endif
#if defined(VRSIM_SHARED_3072) && defined(VRSIM_SHARED_2560)
#error VRSIM_SHARED_3072 and VRSIM_SHARED_2560 are mutually exclusive; define at most one
#endif
#if defined(VRSIM_SHARED_3072) && defined(VRSIM_SHARED_2048)
#error VRSIM_SHARED_3072 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if defined(VRSIM_SHARED_2736) && defined(VRSIM_SHARED_2560)
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2560 are mutually exclusive; define at most one
#endif
#if defined(VRSIM_SHARED_2736) && defined(VRSIM_SHARED_2048)
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if defined(VRSIM_SHARED_2560) && defined(VRSIM_SHARED_2048)
#error VRSIM_SHARED_2560 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
// HIGHRES-H2 mixed-source QA: VRSIM_QA_MIXED_SOURCE selects a 4096x4096 SOURCE
// feeding a frozen 3072x3072 OUTPUT profile, so it is meaningful ONLY together
// with VRSIM_SHARED_3072 (which stays the OUTPUT profile constant). Defining
// the mixed macro without VRSIM_SHARED_3072 is a hard compile-time error.
#if defined(VRSIM_QA_MIXED_SOURCE) && !defined(VRSIM_SHARED_3072)
#error VRSIM_QA_MIXED_SOURCE requires VRSIM_SHARED_3072 (mixed 4096 source into the 3072 output profile)
#endif
inline constexpr uint32_t kStereoBridgeVersionV1 = 1u;
inline constexpr uint32_t kStereoBridgeSlotCountV1 = 3u;
#if defined(VRSIM_QA_MIXED_SOURCE)
// Mixed-source QA build: kStereoBridgeWidthV1/HeightV1 are the SOURCE dimensions
// ONLY (exactly 4096). The XR OUTPUT profile stays 3072 (VRSIM_SHARED_3072);
// that profile is applied by the native owner, not by these source constants.
inline constexpr uint32_t kStereoBridgeWidthV1 = 4096u;
inline constexpr uint32_t kStereoBridgeHeightV1 = 4096u;
#elif defined(VRSIM_SHARED_3072)
inline constexpr uint32_t kStereoBridgeWidthV1 = 3072u;
inline constexpr uint32_t kStereoBridgeHeightV1 = 3072u;
#elif defined(VRSIM_SHARED_2736)
inline constexpr uint32_t kStereoBridgeWidthV1 = 2736u;
inline constexpr uint32_t kStereoBridgeHeightV1 = 2736u;
#elif defined(VRSIM_SHARED_2560)
inline constexpr uint32_t kStereoBridgeWidthV1 = 2560u;
inline constexpr uint32_t kStereoBridgeHeightV1 = 2560u;
#elif defined(VRSIM_SHARED_2048)
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

// INTERNAL telemetry POD -- NOT a wire/export struct; SharedBridgeStatsV1 (72B)
// and every other V1 layout stay untouched. One optional out-sample per
// PrepareOnSourceUI call (default nullptr: existing callsites compile and
// behave identically and incur zero added QPC/counter work). Every field
// describes ONLY real execution of THIS call's D3D9 drain wait: a phase whose
// mask is 0 never happened and must never be read as a measured zero. Tick
// fields are raw QueryPerformanceCounter ticks around the real calls: CPU-side
// wall windows, explicitly NOT GPU execution time. Pure fixed-width numerics:
// no pointers, COM refs, resources or pixels.
struct PrepareWaitSample {
    // ---- real-execution masks: 1 = recorded this call, 0 = never happened --
    uint32_t maskBridgeHresult;   // bridgeHresult holds this call's real return
    uint32_t maskLeftCopy;        // left StretchRect really called
    uint32_t maskRightCopy;       // right StretchRect really called
    uint32_t maskIssue;           // Issue(D3DISSUE_END) really called
    uint32_t maskWaitExecuted;    // the original bounded wait was really entered
    uint32_t maskQpcFrequency;    // qpcFrequency recorded from a real QPF call
    uint32_t maskLeftWall;        // leftCopyWallTicks measured
    uint32_t maskRightWall;       // rightCopyWallTicks measured
    uint32_t maskIssueWall;       // issueWallTicks measured
    uint32_t maskWaitWall;        // waitWallTicks measured
    uint32_t maskGetDataWall;     // getDataWallTicksSum measured
    uint32_t maskSleepWall;       // sleepWallTicksSum measured
    uint32_t maskCopyWallSum;     // copyWallTicksSum = both measured copy walls
    uint32_t lastGetDataHrValid;  // lastGetDataHr is the last real GetData return
    // ---- real HRESULTs ------------------------------------------------------
    LONG bridgeHresult;
    LONG leftCopyHr;
    LONG rightCopyHr;
    LONG issueHr;
    LONG lastGetDataHr;
    // ---- real call counters (fixed width; never inferred from polls) --------
    uint64_t leftCopyCalls;
    uint64_t rightCopyCalls;
    uint64_t issueCalls;
    uint64_t getDataCalls;        // real GetData(NULL,0,FLUSH) invocations
    uint64_t flushGetDataCalls;   // observed FLUSH-flagged calls (this policy: same real calls)
    uint64_t sFalseReturns;       // exact S_FALSE returns only
    uint64_t otherPendingReturns; // other non-failure returns that kept waiting
    uint64_t sleep1Calls;         // only Sleep(1) invocations that really ran
    // ---- raw QPC ticks ------------------------------------------------------
    uint64_t qpcFrequency;
    uint64_t leftCopyWallTicks;
    uint64_t rightCopyWallTicks;
    uint64_t issueWallTicks;
    uint64_t waitWallTicks;        // whole wait window, not per-poll
    uint64_t getDataWallTicksSum;  // sum of real per-GetData call windows
    uint64_t sleepWallTicksSum;    // sum of real per-Sleep(1) windows
    uint64_t copyWallTicksSum;     // tD9copy: left+right completed call walls (CPU wall, NOT GPU time)
};
static_assert(std::is_trivially_copyable<PrepareWaitSample>::value,
              "PrepareWaitSample must stay a trivially copyable numeric POD");

#ifdef VRSIM_QA_D9_BEGIN_POLL
// ---- INTERNAL QA-ONLY Begin/Poll contract (VRSIM_QA_D9_BEGIN_POLL) ----------
// NOT exports, NOT wire structs, NOT ABI: the existing V1 layouts (64B ticket,
// 72B stats), the 24 old exports and the NC01 surface stay the only ABI. A
// later standalone fixture statically links this same TU and calls these
// internal C++ methods. Define OFF: nothing below exists and every existing
// behavior, including the synchronous Prepare semantics and counter meanings,
// is byte-for-byte unchanged. One persistent D9 EVENT exists (cpp d9Ev_), so
// at most ONE pending D9 pair can exist: the Begin/Poll ledger and the old
// synchronous Prepare exclude each other while this define is compiled in.
enum SharedBridgeQaLedgerStateV1 : uint32_t {
    kQaLedgerEmpty = 0,        // no persistent lease (per-call busy/entry counts are NOT this)
    kQaLedgerPreparing = 1,    // owned lease: refs retained, submitted work drain not yet proven
    kQaLedgerQuarantined = 2   // retained unproven lease: terminal until process exit
};

// Persistent EVENT query state of the lease.
enum SharedBridgeQaQueryStateV1 : uint32_t {
    kQaQueryNone = 0,
    kQaQueryArmedPending = 1,      // Issue END really happened; drain not proven
    kQaQueryCompleteProven = 2,    // a real GetData S_OK proved THIS event's submitted work
    kQaQueryUnprovenRetained = 3   // failed/unknown/budget: retained, never inferred
};

// Own-ref presence bits (numeric mirrors; the COM pointers themselves are
// source-thread-only and never published).
enum SharedBridgeQaRefBitsV1 : uint32_t {
    kQaRefLeftSurface = 1u,
    kQaRefRightSurface = 2u,
    kQaRefDevice = 4u,
    kQaRefParent = 8u
};

// Synthetic fault plan bits: QA branch selection ONLY. A fault never fakes a
// real HRESULT: skipped real calls stay unrecorded (mask 0) and synthetic
// errors after real GPU submission retain the unproven work.
enum SharedBridgeQaFaultV1 : uint32_t {
    kQaFaultSkipLeftCopy = 1u,              // skip the real left StretchRect (pre-work rollback; nothing submitted)
    kQaFaultSkipRightCopy = 2u,             // skip the real right StretchRect after a successful left (still Issue+drain the left)
    kQaFaultSkipIssueAfterSubmit = 4u,      // skip the real Issue: submitted work unproven -> quarantine/retain
    kQaFaultSkipGetDataAfterSubmit = 8u,    // skip the real GetData: drain unproven -> quarantine/retain
    kQaFaultForceDeadlineExceeded = 16u,    // no real GetData; synthetic budget branch -> quarantine/retain
    kQaFaultRetainInsteadOfGrant = 32u,     // real drain S_OK proven, but refuse cleanup/grant -> quarantine/retain
    kQaFaultSimulateFpuRestoreUnproven = 64u // run the REAL restore, then refuse to trust it -> quarantine/retain
};

// Quarantine reason codes (fixed integers; 0 = none).
// 1 capture unproven, 2 issue failed/skipped after submit, 3 GetData failed/
// unknown/skipped, 4 GetData/128-call/1000ms budget exhausted (real or
// synthetic), 5 QPC timing unavailable with live work, 6 unproven FPU restore,
// 7 owned-ref cleanup not proven, 8 pre-work release not clean, 9 exception,
// 10 internal state contradiction, 11 refused grant (synthetic fault).
enum SharedBridgeQaQuarantineCodeV1 : uint32_t {
    kQaQuarantineNone = 0
};

inline constexpr uint32_t kStereoBridgeQaStructVersionV1 = 1u;

// Source stamp numbers captured with Begin. They are retained verbatim as
// preservation proof ONLY; this card does NOT validate PoseSample/capture
// identity against them, and no new stamp/pose may ever be re-bound to old RGB.
struct QaSourceStampV1 {
    uint32_t version;     // kStereoBridgeQaStructVersionV1
    uint32_t structBytes; // sizeof(QaSourceStampV1)
    uint64_t frameSerial;
    uint64_t poseSerial;
    uint64_t captureBeginQpc;
};
static_assert(std::is_trivially_copyable<QaSourceStampV1>::value, "POD");

// Exact internal lease key. slot + Init-fixed epoch/generation + strict
// monotonic nonzero nonce + struct version/bytes. Never a pointer, never
// reused, never rebound while a lease lives.
struct QaBeginTokenV1 {
    uint32_t version;
    uint32_t structBytes;
    uint32_t slotIndex;
    uint32_t generation;
    uint64_t epoch;
    uint64_t nonce; // 0 = invalid
};
static_assert(std::is_trivially_copyable<QaBeginTokenV1>::value, "POD");
static_assert(sizeof(QaBeginTokenV1) == 32, "QaBeginTokenV1 layout");

// Distinct result statuses: a real pending is never an old PREPARED ticket.
enum SharedBridgeQaStatusV1 : uint32_t {
    kQaStatusNone = 0,
    kQaStatusRejected = 1,            // refused call: zero GPU work, zero true-lease mutation
    kQaStatusPreWorkRolledBack = 2,   // nothing submitted: proven-clean pre-work rollback
    kQaStatusPending = 3,             // real S_FALSE: ledger + owned refs + PREPARING retained
    kQaStatusImmediateGranted = 4,    // Begin's one real GetData S_OK; PREPARED ticket granted
    kQaStatusCompletedGranted = 5,    // Poll's real GetData S_OK; PREPARED ticket granted
    kQaStatusCompletedNoGrant = 6,    // real drain S_OK but cancelled/closed: cleanup, FREE, NO ticket
    kQaStatusFailedDrained = 7,       // pair failure: submitted work drained, cleaned up, NO ticket
    kQaStatusQuarantinedRetained = 8  // unproven: refs retained, terminal non-zero, never FREE
};

struct QaBeginResultV1 {
    uint32_t version;
    uint32_t structBytes;
    uint32_t status;              // SharedBridgeQaStatusV1
    LONG bridgeHresult;           // the real returned HRESULT
    uint32_t copyExecMask;        // bit0/1 = REAL StretchRect executed (synthetic skips keep 0)
    uint32_t copyOkMask;          // bit0/1 = REAL StretchRect returned success
    uint32_t faultConsumed;       // fault-plan bits actually consumed by THIS call
    uint32_t ledgerStateAfter;    // SharedBridgeQaLedgerStateV1 after the call
    LONG firstFailureHr;          // FIRST real failing HRESULT kept verbatim (0 = none)
    LONG lastIssueHr;             // last REAL Issue result (never recorded if skipped)
    LONG lastGetDataHr;           // last REAL GetData(FLUSH) result (never recorded if skipped)
    uint32_t getDataCallsThisCall; // REAL GetData calls made by Begin (0 or 1)
    QaBeginTokenV1 token;         // valid ONLY when status==Pending; zero otherwise
};
static_assert(std::is_trivially_copyable<QaBeginResultV1>::value, "POD");
static_assert(sizeof(QaBeginResultV1) == 80, "QaBeginResultV1 layout");

struct QaPollResultV1 {
    uint32_t version;
    uint32_t structBytes;
    uint32_t status;               // SharedBridgeQaStatusV1
    LONG bridgeHresult;            // S_OK granted / S_FALSE pending / non-zero otherwise
    LONG lastGetDataHr;            // THIS call's single REAL GetData result (0 = never called)
    LONG firstFailureHr;           // preserved first real failure (0 = none)
    uint32_t getDataCallsThisCall; // 0 or 1: Poll makes AT MOST one real GetData
    uint32_t pairOkMask;           // copy-ok mask mirror at return
    uint64_t elapsedMs;            // real QPC elapsed since Begin for THIS call
    uint64_t getDataCallsTotal;    // cumulative REAL GetData across the whole lease (never reset)
    uint32_t ticketGranted;        // 1 only with a granted ordinary PREPARED ticket
    uint32_t ledgerStateAfter;
};
static_assert(std::is_trivially_copyable<QaPollResultV1>::value, "POD");

// Pure fixed-width numeric snapshot for later fixtures: no pointers, no COM
// refs, no strings, no allocation. Callable from either thread; every field is
// an Interlocked mirror read under the short shared lifetime gate.
struct QaBridgeSnapshotV1 {
    uint32_t version;
    uint32_t structBytes;
    uint32_t ledgerState;           // SharedBridgeQaLedgerStateV1
    uint32_t slotValid;             // 1 = lease occupies a slot
    uint32_t slotIndex;
    uint32_t quarantined;
    uint32_t admissionClosed;
    uint32_t initializedNow;
    uint32_t finalizing;
    uint32_t finalized;
    uint64_t epoch;                 // Init-fixed epoch the lease is bound to
    uint64_t generation;            // Init-fixed generation (mirrored 64-bit)
    uint64_t nonce;                 // 0 = none; monotonic, never reused
    uint64_t stampFrame;
    uint64_t stampPose;
    uint64_t stampCaptureBeginQpc;
    uint64_t actualCopyCalls;       // cumulative REAL StretchRect calls in the lease
    uint64_t actualIssueCalls;      // cumulative REAL Issue calls in the lease
    uint64_t actualGetDataCalls;    // cumulative REAL GetData(FLUSH) calls in the lease
    LONG firstFailureHr;            // first real failing HRESULT (0 = none)
    LONG lastIssueHr;               // last REAL Issue result (0 = none recorded)
    LONG lastGetDataHr;             // last REAL GetData result (0 = none recorded)
    LONG firstGetDataHr;            // Begin's first REAL GetData result (0 = none)
    uint32_t copyExecMask;
    uint32_t copyOkMask;
    uint32_t timingOk;              // QPC frequency+start proven for this lease
    uint64_t qpcFrequency;
    uint64_t startQpc;
    uint64_t lastPollElapsedMs;
    uint32_t cancelRequested;
    uint32_t ticketGranted;         // sticky for the LAST resolved lease
    uint64_t grantedTicket;         // ticket number of the last granted lease (0 = none)
    uint32_t queryState;            // SharedBridgeQaQueryStateV1
    uint32_t refsHeldMask;          // SharedBridgeQaRefBitsV1 numeric mirror
    uint32_t quarantineCode;        // SharedBridgeQaQuarantineCodeV1 semantics above
    uint32_t faultPlan;             // last set synthetic fault mask
    uint32_t faultConsumed;         // bits consumed by the last Begin/Poll
    uint32_t lastStatus;            // last QA result status
    uint64_t qaGrantCount;          // PREPARED tickets granted via QA Begin/Poll
    uint64_t qaRejectCount;         // refused QA calls (leases never mutated)
    uint64_t qaPendingCount;        // real S_FALSE observations
    uint64_t qaRetainedCount;       // quarantine/retain events
    uint64_t qaCancelCount;
    uint64_t sourceBusy;            // per-call guard mirror (NOT an empty-lease proof)
    uint64_t workerBusy;
    uint64_t activeEntries;
    uint64_t initCount;
    uint64_t prepareCount;          // old counter, old meaning: QA grants do NOT touch it
    uint64_t commitCount;
    uint64_t dropCount;
    uint64_t drainCount;
    uint64_t quarantineCount;
    uint32_t slotState[kStereoBridgeSlotCountV1];
};
static_assert(std::is_trivially_copyable<QaBridgeSnapshotV1>::value, "POD");
#ifdef VRSIM_QA_D9_GPU_TIMING
// ---- INTERNAL QA-ONLY D9_RIGHT_COPY GPU timing observer (nested guard:
// VRSIM_QA_D9_BEGIN_POLL + VRSIM_QA_D9_GPU_TIMING) -----------------------------
// Pure numeric POD: no export, no wire, no ABI, no layout change to anything
// above. The observer times ONLY the real right-eye StretchRect copy stage on
// the ACTUAL existing source D3D9Ex device via device-local TIMESTAMP queries.
// It never covers the opaque SDK rendering, the left copy, the whole stereo
// pair, or a pure completion delay. A query's observation-ready moment is NOT
// a GPU completion time. All GPU tick fields are raw same-source-device
// timestamp values; QPC, GPU and cross-device clocks are NEVER subtracted.
inline constexpr uint32_t kQaD9GpuObsSchemaV1 = 1u;
inline constexpr uint32_t kQaD9GpuObsCapacityV1 = 2048u;  // fixed POD record ring
inline constexpr uint32_t kQaD9GpuObsSlotCountV1 = 2u;    // preallocated query slots
inline constexpr uint32_t kQaD9GpuObsSampleEveryV1 = 16u; // at most every 16th eligible txn
// stage code: 1 = D9_RIGHT_COPY (the ONLY stage this observer can ever record)
inline constexpr uint32_t kQaD9GpuObsStageRightCopyV1 = 1u;

// One fixed-width observation record. No pointers, no COM refs, no surfaces.
// A mask/validity bit of 0 means NEVER HAPPENED/UNAVAILABLE, never a measured
// zero; unavailable timing or query results are invalid, not 0ms.
struct QaD9GpuObsRecordV1 {
    uint32_t version;
    uint32_t structBytes;
    uint32_t stage;            // always kQaD9GpuObsStageRightCopyV1
    uint32_t obsSlot;          // observer query slot 0/1
    uint32_t leaseSlot;        // ORIGINAL bridge lease slot
    uint32_t sourceThreadId;   // ACTUAL SourceUI OS thread id at bracket time
    uint64_t epoch;            // ORIGINAL immutable lease stamp, verbatim
    uint32_t generation;
    uint32_t pad0;
    uint64_t frameSerial;
    uint64_t poseSerial;
    uint64_t captureBeginQpc;
    uint64_t cpuBeginQpc;      // CPU QPC bracket around the observer commands
    uint64_t cpuEndQpc;
    uint64_t readyQpc;         // observation-ready QPC: NOT GPU completion time
    uint32_t qpcMask;          // bit0 begin, bit1 end, bit2 ready actually measured
    uint32_t pad1;
    uint64_t gpuBeginTicks;    // raw TIMESTAMP values, same source device only
    uint64_t gpuEndTicks;
    uint64_t gpuFreqTicks;     // raw TIMESTAMPFREQ value, real positive or absent
    uint64_t gpuDeltaTicks;    // end-begin ONLY when validity==1
    int32_t  disjointValue;    // -1 unknown, 0 FALSE, 1 TRUE (real Win32 BOOL)
    uint32_t issueCmdMask;     // bit0 freq, bit1 disjointBegin, bit2 begin, bit3 end, bit4 disjointEnd (real successes)
    int32_t  firstIssueHr;     // actual FIRST observer Issue failure (0 = none)
    int32_t  rightCopyHr;      // real right StretchRect HRESULT in sample status (rightCopyHrIsReal=1)
    uint32_t rightCopyHrIsReal;
    int32_t  gdHrFreq;         // actual GetData result per field; 0 = never called (never called unissued)
    int32_t  gdHrBegin;
    int32_t  gdHrEnd;
    int32_t  gdHrDisjoint;
    uint32_t dataMask;         // bit0 freq, bit1 begin, bit2 end, bit3 disjoint: real S_OK result read
    uint32_t validity;         // 1 = non-disjoint, actual positive frequency, ordered timestamps, all results read
    uint32_t resolved;         // 1 = final verdict reached; 0 = incomplete row cached at ORIGINAL finalize
    uint32_t unused0;
};
static_assert(std::is_trivially_copyable<QaD9GpuObsRecordV1>::value, "POD");

// Pure cached aggregate for the QA-only internal accessor. Counters are real
// observations: skippedBusy = eligible 16th sampling with no free slot;
// unsupported/partial creation failure and unproven slots disable observation
// (disabledReason 1 partial creation, 2 unsupported, 3 all slots retired,
// 4 ORIGINAL session finalized) and never fail or alter product work.
struct QaD9GpuObsCacheV1 {
    uint32_t version;
    uint32_t structBytes;
    uint32_t enabled;          // 1 = observation armed; 0 = disabled
    uint32_t disabledReason;
    uint64_t eligibleRightCopyTxns;  // QaSubmitDrain entries whose REAL right copy executed
    uint64_t bracketsSampled;
    uint64_t skippedBusy;
    uint64_t issueFailed;      // brackets with at least one real observer Issue failure
    uint64_t resolveFailed;    // GetData marked the slot invalid/unproven
    uint64_t incomplete;       // invalid verdicts + rows cached at finalize
    uint64_t validRecords;     // same-device valid non-disjoint ordered deltas
    uint64_t overflowRecords;  // ring full: rows lost, capture INCOMPLETE
    uint64_t resolveGetDataCalls; // real observer GetData invocations
    uint64_t recordCount;      // records actually retained (<= capacity)
    uint64_t gpuFreqTicks;     // last validly observed real frequency
    uint64_t qpcFreqTicks;     // real QPC frequency at observer init (0 = unavailable)
    uint64_t deltaTicksSum;
    uint64_t deltaTicksMin;    // 0xFFFFFFFFFFFFFFFF = none yet
    uint64_t deltaTicksMax;
    uint32_t finalizedCached;  // 1 = ORIGINAL finalize cached incomplete rows and released observers
    uint32_t lastRecordValid;  // 1 = lastRecord holds a real terminal record
    QaD9GpuObsRecordV1 lastRecord;
};
static_assert(std::is_trivially_copyable<QaD9GpuObsCacheV1>::value, "POD");
#endif // VRSIM_QA_D9_GPU_TIMING
#endif // VRSIM_QA_D9_BEGIN_POLL

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
    // readback, map, staging or encode anywhere. The optional waitSample
    // (default nullptr) receives ONLY observations of this call's real D9
    // drain wait execution; it never changes the wait policy, grants, FPU
    // gating or quarantine outcomes, and a null pointer adds no sampling work.
    HRESULT PrepareOnSourceUI(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                              SharedPreparedPairV1* out, PrepareWaitSample* waitSample = nullptr);

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

#ifdef VRSIM_QA_D9_BEGIN_POLL
    // ---- INTERNAL QA-only single-pending Begin/Poll surface ------------------
    // Internal C++ methods on this class ONLY: no C ABI export, no wire struct,
    // no ABI change. The later fixture statically links this TU. Every entry
    // requires the original source UI thread; the whole existing Initialize /
    // Prepare / Commit / worker / finalize machinery, gates and layouts stay
    // exactly as reviewed. Driver calls can still occupy CPU wall time; no hard
    // real-time, FPS or comfort claim exists here.

    // Begin: original same-source-thread + FREE-slot + open-admission +
    // empty-ledger gates claimed atomically under the short grant lock, then
    // the original AcquireSources/canonical/desc/parent gates with the caller's
    // borrowed numbers while still valid, per-call verified FPU capture before
    // any COM, both StretchRects into the ORIGINAL pool slot, exactly ONE
    // Issue(D3DISSUE_END) on the single persistent EVENT, then at most ONE
    // real GetData(nullptr,0,D3DGETDATA_FLUSH). S_FALSE retains the owned
    // references and PREPARING and returns a token with ZERO ticket granted;
    // an immediate real S_OK may grant one ordinary PREPARED ticket via
    // outTicket. Duplicate/pending or wrong-gate calls are refused with zero
    // QI/copy/Issue and zero true-lease mutation. The first real failing
    // HRESULT is preserved; pre-work failures roll back only proven-clean.
    HRESULT QaBeginOnSourceUI(uint32_t slotIndex, uint64_t leftSurface, uint64_t rightSurface,
                              const QaSourceStampV1& stamp, QaBeginResultV1* result,
                              SharedPreparedPairV1* outTicket);

    // Poll: exact token key (slot/epoch/generation/nonce/version/bytes) on the
    // same source thread; at most ONE real GetData(FLUSH) per call, no
    // Sleep/spin/pump/render/re-Issue/re-acquire/allocation. QPC start and
    // frequency were acquired once at Begin; the 128-real-GetData and 1000ms
    // budgets are cumulative across calls and never reset by Poll/Cancel.
    // S_FALSE retains; a real S_OK proves the previously submitted work, then
    // cleanup runs inside this call's FPU capture with restore verified BEFORE
    // any grant; an ordinary PREPARED ticket is granted only with both copies
    // proven, exact lease, cleanup+FPU verified and admission still open under
    // the original short grant lock. Failed/unknown/budget/deadline outcomes
    // quarantine and retain with a non-zero terminal result.
    HRESULT QaPollOnSourceUI(const QaBeginTokenV1& token, QaPollResultV1* result,
                             SharedPreparedPairV1* outTicket);

    // Exact-key cancel: closes admission (idempotent like CloseAdmission) and
    // marks the original lease cancelRequested. It never frees, never proves
    // and never discards the lease; a wrong key closes/marks NOTHING. A closed
    // or cancelled lease may only drain/clean up or roll back to FREE.
    HRESULT QaRequestCancelOnSourceUI(const QaBeginTokenV1& token, QaBeginResultV1* result);

    // Synthetic fault plan (QA branch selection only): settable ONLY on the
    // source thread while the ledger is empty; consumed bits are recorded
    // separately from real call counts/results. Injected faults after real GPU
    // submission retain the unproven work; nothing here fabricates a real
    // S_FALSE/S_OK as pending/completion evidence.
    HRESULT QaSetFaultPlan(uint32_t faultMask);

    // Pure numeric cached snapshot; short shared lifetime gate, Interlocked
    // mirror reads only, no COM/driver work, callable from either thread.
    void QaSnapshot(QaBridgeSnapshotV1* out) const noexcept;
#ifdef VRSIM_QA_D9_GPU_TIMING
    // QA-only INTERNAL pure cached accessor (nested VRSIM_QA_D9_BEGIN_POLL +
    // VRSIM_QA_D9_GPU_TIMING guard): no C export, no PInvoke, no ABI. Short
    // shared lifetime gate + numeric reads only; makes ZERO COM/GPU/SDK calls.
    // The native QA trace consumer calls it only after the ORIGINAL lifetime
    // finalization/Destroy (lifecycle-serialized read of the source-thread
    // written numeric cache); the aggregate counters are Interlocked.
    void QaD9GpuTimingCache(QaD9GpuObsCacheV1* out) const noexcept;
    // QA-only INTERNAL indexed cached copy (no C export, no PInvoke, no ABI):
    // returns TRUE plus ONE retained numeric POD only for an index below the
    // finalizedCached-gated retained count. SHORT shared lifetime gate, pure
    // POD copy: zero COM/GPU/SDK/query calls, zero allocation, never a
    // full-ring hot-path transfer.
    bool QaD9GpuTimingRecord(uint32_t index, QaD9GpuObsRecordV1* out) const noexcept;
#endif
#endif // VRSIM_QA_D9_BEGIN_POLL

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
