/*
 * NC02 dual-slot D3D9 pair staging helper - public C ABI (x64, cdecl, pack 8).
 *
 * Single owned context per process, pinned to the UI thread that called
 * NC02_PairStage_CreateV1. Handles are registry IDs (start at 1, never
 * reused), never raw pointers. No C++ types cross the ABI.
 *
 * Return-code convention (int32_t):
 *   0   success for every entry point
 *   1   NC02_PairStage_SubmitPairV1: Full (no slot free; nothing overwritten)
 *       NC02_PairStage_PollCollectV1: Pending / Empty (no pixels written)
 *       NC02_PairStage_StopStepV1: DrainPending / Busy
 *   <0  failure. Sentinel codes below, or the real failing HRESULT
 *       preserved as a negative int32_t. S_FALSE is never treated as done;
 *       callers must not add the tick fields together as a total cost.
 */
#ifndef NC02_PAIR_STAGE_H_
#define NC02_PAIR_STAGE_H_

#include <stdint.h>

#ifdef _WIN32
#  ifdef NC02_PAIR_STAGE_EXPORTS
#    define NC02_API __declspec(dllexport)
#  else
#    define NC02_API __declspec(dllimport)
#  endif
#else
#  define NC02_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

/* PairToken, 40 bytes. pairSeq must be non-zero and strictly increasing. */
typedef struct NC02PairTokenV1 {
    uint32_t structBytes;      /* must be 40  (offset 0)  */
    uint32_t version;          /* must be 1   (offset 4)  */
    uint64_t pairSeq;          /* offset 8                */
    uint64_t poseId;           /* offset 16               */
    uint64_t epoch;            /* offset 24               */
    uint64_t ownerGeneration;  /* offset 32               */
} NC02PairTokenV1;

/* Collected, 176 bytes. status == 0 means Complete and pixels are valid.
 * All *Ticks and QPC values are host QueryPerformanceCounter units; the
 * caller records QueryPerformanceFrequency. submitQpc / collectQpc are
 * absolute QPC stamps. */
typedef struct NC02CollectedPairV1 {
    NC02PairTokenV1 token;              /* frozen token from Submit (offset 0)  */
    uint32_t slotIndex;                 /* 0 / 1                    (offset 40) */
    uint32_t status;                    /* 0 = Complete             (offset 44) */
    uint64_t leftSourceIdentity;        /* borrowed L at Submit     (offset 48) */
    uint64_t rightSourceIdentity;       /* borrowed R at Submit     (offset 56) */
    uint64_t deviceIdentity;            /* owned device ref         (offset 64) */
    uint32_t width;                     /*                          (offset 72) */
    uint32_t height;                    /*                          (offset 76) */
    uint64_t submitQpc;                 /* absolute                 (offset 80) */
    uint64_t collectQpc;                /* absolute                 (offset 88) */
    uint64_t getRenderTargetDataTicks;  /* sum L/R                  (offset 96) */
    uint64_t issueTicks;                /* Issue END only           (offset 104) */
    uint64_t queryTicks;                /* incl. waits, host units  (offset 112) */
    uint64_t lockTicks;                 /* all attempts             (offset 120) */
    uint64_t rowCopyTicks;              /*                          (offset 128) */
    uint64_t unlockTicks;               /* all attempts             (offset 136) */
    uint64_t submitTotalTicks;          /* whole Submit call        (offset 144) */
    uint64_t pollTotalTicks;            /* sum of all Poll calls    (offset 152) */
    uint32_t queryAttempts;             /*                          (offset 160) */
    uint32_t leftLockAttempts;          /*                          (offset 164) */
    uint32_t rightLockAttempts;         /*                          (offset 168) */
    uint32_t reserved;                  /*                          (offset 172) */
} NC02CollectedPairV1;

/* Stop state, 56 bytes. createTicks / releaseTicks are main-thread
 * QueryPerformanceCounter durations. lastError keeps the first sticky
 * failure and is never overwritten by later Pending states. */
typedef struct NC02StopStateV1 {
    uint32_t structBytes;           /* must be 56   (offset 0)  */
    uint32_t version;               /* must be 1    (offset 4)  */
    uint32_t admissionOpen;         /*              (offset 8)  */
    uint32_t callBusy;              /*              (offset 12) */
    uint32_t pendingSlots;          /*              (offset 16) */
    uint32_t liveOwnedResourceRefs; /*              (offset 20) */
    uint32_t stopVerified;          /*              (offset 24) */
    uint32_t faulted;               /*              (offset 28) */
    int32_t lastError;              /*              (offset 32) */
    uint32_t reserved;              /*              (offset 36) */
    uint64_t createTicks;           /*              (offset 40) */
    uint64_t releaseTicks;          /*              (offset 48) */
} NC02StopStateV1;

#pragma pack(pop)

#define NC02_PAIR_TOKEN_BYTES     40u
#define NC02_COLLECTED_BYTES      176u
#define NC02_STOP_BYTES           56u
#define NC02_COLLECTED_STATUS_COMPLETE 0u

/* Negative sentinel failure codes. Real HRESULT failures are returned
 * as (int32_t)hr, which never collides with these values. */
#define NC02_E_INVALIDARG    (-1)  /* bad pointer / capacity / range     */
#define NC02_E_UNSUPPORTED   (-2)  /* desc, format or shape unsupported  */
#define NC02_E_BUSY          (-3)  /* another call is active (reentry)   */
#define NC02_E_THREAD        (-4)  /* wrong thread; no D3D / pixel work  */
#define NC02_E_HANDLE        (-5)  /* unknown / stale handle             */
#define NC02_E_STOPPED       (-6)  /* admission closed / stop requested  */
#define NC02_E_TOKEN         (-7)  /* token / epoch / gen / seq invalid  */
#define NC02_E_FAULT         (-8)  /* sticky fault on the context        */
#define NC02_E_CPP_EXCEPTION (-9)  /* C++ exception, never propagates    */

/* Borrowed source surface is validated by GetDesc/GetDevice and is only
 * used inside this synchronous call stack; it is never AddRef'd, stored
 * or released. Out-param is written only on success. This makes no safety
 * promise for an arbitrary/invalid source pointer: the caller must pass a
 * live IDirect3DSurface9 it keeps valid for the whole call. */
NC02_API int32_t NC02_PairStage_CreateV1(
    void* borrowedSource,
    uint32_t expectedW,
    uint32_t expectedH,
    uint32_t expectedFormat,
    uint64_t epoch,
    uint64_t generation,
    uint64_t* outHandle);

/* TokenBytes must be exactly 40. Both borrowed surfaces must be distinct,
 * match Create-time shape and belong to the owned device. Returns 0
 * (Accepted), 1 (Full) or negative. */
NC02_API int32_t NC02_PairStage_SubmitPairV1(
    uint64_t handle,
    void* borrowedLeft,
    void* borrowedRight,
    const NC02PairTokenV1* token,
    uint32_t tokenBytes);

/* Bounded single-step collect of the oldest Accepted pair. At most one
 * GetData(FLUSH) per call, no internal loops or waits. resultCapacity must
 * be exactly 176. Returns 0 (Complete), 1 (Pending/Empty) or negative.
 * Only S_OK marks a query ready; S_FALSE is Pending; an unexpected positive
 * is a fault (-8) and is never treated as done. Negative results that occur
 * before the copy stage (bad parameters, gated fault/stop, left/right busy
 * WASSTILLDRAWING, left/right real-HRESULT lock failures, invalid pitch after
 * both unlocks) write no pixels. Only a failure after the row copy (an Unlock
 * error, or a late Stop/fault seen after both unlocks succeeded) may leave
 * bytes already written into outL/outR; those buffers must then be treated as
 * invalid and the pair is never published as Complete. */
NC02_API int32_t NC02_PairStage_PollCollectV1(
    uint64_t handle,
    uint8_t* outL,
    uint64_t capL,
    uint8_t* outR,
    uint64_t capR,
    NC02CollectedPairV1* result,
    uint32_t resultCapacity);

/* Finite stop step: closes admission, drains at most one FLUSH GetData per
 * pending slot, releases owned resources in reverse creation order only
 * when nothing is in flight. Returns 0 (StopVerified), 1 (DrainPending/
 * Busy) or negative. Never touches SDK source objects. */
NC02_API int32_t NC02_PairStage_StopStepV1(
    uint64_t handle,
    NC02StopStateV1* result,
    uint32_t resultCapacity);

/* Frees the context only after StopVerified with refs/pending/busy all 0.
 * After success the handle is dead forever and the ID is never reused. */
NC02_API int32_t NC02_PairStage_DestroyV1(uint64_t handle);

#ifdef __cplusplus
}  /* extern "C" */

#include <cstddef>

/* ABI layout guards (C++17). sizeof for these all-natural-alignment
 * structs equals the packed size; offsets are asserted explicitly. */
static_assert(sizeof(NC02PairTokenV1) == 40, "NC02 token must be 40 bytes");
static_assert(offsetof(NC02PairTokenV1, structBytes) == 0, "t0");
static_assert(offsetof(NC02PairTokenV1, version) == 4, "t4");
static_assert(offsetof(NC02PairTokenV1, pairSeq) == 8, "t8");
static_assert(offsetof(NC02PairTokenV1, poseId) == 16, "t16");
static_assert(offsetof(NC02PairTokenV1, epoch) == 24, "t24");
static_assert(offsetof(NC02PairTokenV1, ownerGeneration) == 32, "t32");

static_assert(sizeof(NC02CollectedPairV1) == 176, "NC02 collected must be 176 bytes");
static_assert(offsetof(NC02CollectedPairV1, token) == 0, "c0");
static_assert(offsetof(NC02CollectedPairV1, slotIndex) == 40, "c40");
static_assert(offsetof(NC02CollectedPairV1, status) == 44, "c44");
static_assert(offsetof(NC02CollectedPairV1, leftSourceIdentity) == 48, "c48");
static_assert(offsetof(NC02CollectedPairV1, rightSourceIdentity) == 56, "c56");
static_assert(offsetof(NC02CollectedPairV1, deviceIdentity) == 64, "c64");
static_assert(offsetof(NC02CollectedPairV1, width) == 72, "c72");
static_assert(offsetof(NC02CollectedPairV1, height) == 76, "c76");
static_assert(offsetof(NC02CollectedPairV1, submitQpc) == 80, "c80");
static_assert(offsetof(NC02CollectedPairV1, collectQpc) == 88, "c88");
static_assert(offsetof(NC02CollectedPairV1, getRenderTargetDataTicks) == 96, "c96");
static_assert(offsetof(NC02CollectedPairV1, issueTicks) == 104, "c104");
static_assert(offsetof(NC02CollectedPairV1, queryTicks) == 112, "c112");
static_assert(offsetof(NC02CollectedPairV1, lockTicks) == 120, "c120");
static_assert(offsetof(NC02CollectedPairV1, rowCopyTicks) == 128, "c128");
static_assert(offsetof(NC02CollectedPairV1, unlockTicks) == 136, "c136");
static_assert(offsetof(NC02CollectedPairV1, submitTotalTicks) == 144, "c144");
static_assert(offsetof(NC02CollectedPairV1, pollTotalTicks) == 152, "c152");
static_assert(offsetof(NC02CollectedPairV1, queryAttempts) == 160, "c160");
static_assert(offsetof(NC02CollectedPairV1, leftLockAttempts) == 164, "c164");
static_assert(offsetof(NC02CollectedPairV1, rightLockAttempts) == 168, "c168");
static_assert(offsetof(NC02CollectedPairV1, reserved) == 172, "c172");

static_assert(sizeof(NC02StopStateV1) == 56, "NC02 stop must be 56 bytes");
static_assert(offsetof(NC02StopStateV1, structBytes) == 0, "s0");
static_assert(offsetof(NC02StopStateV1, version) == 4, "s4");
static_assert(offsetof(NC02StopStateV1, admissionOpen) == 8, "s8");
static_assert(offsetof(NC02StopStateV1, callBusy) == 12, "s12");
static_assert(offsetof(NC02StopStateV1, pendingSlots) == 16, "s16");
static_assert(offsetof(NC02StopStateV1, liveOwnedResourceRefs) == 20, "s20");
static_assert(offsetof(NC02StopStateV1, stopVerified) == 24, "s24");
static_assert(offsetof(NC02StopStateV1, faulted) == 28, "s28");
static_assert(offsetof(NC02StopStateV1, lastError) == 32, "s32");
static_assert(offsetof(NC02StopStateV1, reserved) == 36, "s36");
static_assert(offsetof(NC02StopStateV1, createTicks) == 40, "s40");
static_assert(offsetof(NC02StopStateV1, releaseTicks) == 48, "s48");

#endif /* __cplusplus */

#endif /* NC02_PAIR_STAGE_H_ */
