// SharedStereoWire.h — HIGHRES-H2SH-CARD1: additive Pack8 shared-stereo wire.
// Frozen originals (GpuProjectionPass.h Plan56/Stats120, SharedStereoBridge.h
// ticket64/SharedBridgeStatsV1) are unchanged and merely re-asserted here.
// This header adds the NEW capture identity192 and stop-state32 layouts, the
// stable export return-code table, zeroed serializers for every native-
// produced wire buffer (implicit padding is ALWAYS zeroed, never compared as
// uninitialized bytes), and field-by-field identity/ticket comparators.
// Identity comparison is field-wise including the IEEE bit pattern of each
// finite yaw/plan double; a whole-struct memcmp against caller storage is
// forbidden. x64 MSVC, C++17.

#ifndef VRSIM_SHARED_STEREO_WIRE_H
#define VRSIM_SHARED_STEREO_WIRE_H

#ifndef _M_X64
#error HIGHRES-H2SH SharedStereoWire requires x64
#endif

#include <cstring>

#include "GpuProjectionPass.h"    // frozen ProjectionPlanV1 (56 B)
#include "SharedStereoBridge.h"   // frozen SharedPreparedPairV1 / SharedBridgeStatsV1

namespace VRSIM {

// ---- stable export result codes for every VRSIM_*Shared*Stereo*V1 entry ----
// 0 = completed, 1 = pending / not completed; negatives are distinct honest
// failures. Bridge HRESULTs are preserved internally (lastBridgeHresult) and a
// bridge S_FALSE is NEVER mapped to a completed return.
enum SharedExportResultV1 : int {
    kSharedExportCompleted    = 0,
    kSharedExportPending      = 1,
    kSharedExportBadArgument  = -1,
    kSharedExportUnavailable  = -2,  // activation gate disabled / bridge session not bound
    kSharedExportStale        = -3,  // admission closed, epoch/generation moved, ticket/identity not current
    kSharedExportQuarantined  = -4,  // unproven GPU work: resources retained, no reuse
    kSharedExportError        = -5   // internal failure with preserved bridge HRESULT
};

constexpr uint32_t kSharedWireVersionV1 = 1u;
constexpr uint32_t kSharedTicketExportBytesV1 = 64u;
constexpr uint32_t kSharedCaptureIdentityV1Bytes = 192u;
constexpr uint32_t kSharedCaptureIdentityV1Version = 1u;
constexpr uint32_t kSharedStopStateV1Bytes = 32u;
constexpr uint32_t kSharedStopStateV1Version = 1u;
constexpr uint32_t kSharedBridgeStatsExportBytesV1 = 72u;

// SharedStopStateV1::flags cached bits (never inferred from elapsed time).
enum SharedStopFlagV1 : uint32_t {
    kSharedStopFlagAdmissionClosed  = 1u,
    kSharedStopFlagWorkerDrained    = 2u,
    kSharedStopFlagSourceFinalized  = 4u,
    kSharedStopFlagQuarantined      = 8u
};

// ============================================================================
// Re-assertion of the FROZEN layouts (proofs only; no field changes).
// ticket64: version0/bytes4/slot8/gen12; epoch16/ticket24/legacyL32/legacyR40;
// width48/height52/format88 at 56; implicit tail padding 60..63 is serialized
// zeroed and never participates in comparison.
// ============================================================================
static_assert(sizeof(SharedPreparedPairV1) == 64, "shared ticket stays 64 bytes");
static_assert(offsetof(SharedPreparedPairV1, version) == 0, "ticket.version at 0");
static_assert(offsetof(SharedPreparedPairV1, structBytes) == 4, "ticket.structBytes at 4");
static_assert(offsetof(SharedPreparedPairV1, slotIndex) == 8, "ticket.slotIndex at 8");
static_assert(offsetof(SharedPreparedPairV1, generation) == 12, "ticket.generation at 12");
static_assert(offsetof(SharedPreparedPairV1, epoch) == 16, "ticket.epoch at 16");
static_assert(offsetof(SharedPreparedPairV1, ticket) == 24, "ticket.ticket at 24");
static_assert(offsetof(SharedPreparedPairV1, legacyLeft) == 32, "ticket.legacyLeft at 32");
static_assert(offsetof(SharedPreparedPairV1, legacyRight) == 40, "ticket.legacyRight at 40");
static_assert(offsetof(SharedPreparedPairV1, width) == 48, "ticket.width at 48");
static_assert(offsetof(SharedPreparedPairV1, height) == 52, "ticket.height at 52");
static_assert(offsetof(SharedPreparedPairV1, format88) == 56, "ticket.format88 at 56");
static_assert(sizeof(SharedPreparedPairV1) - offsetof(SharedPreparedPairV1, format88) == 8,
              "ticket tail padding range is exactly bytes 56..63 (4 format bytes + 4 zeroed)");

// exported SharedBridgeStatsV1 (72 B): version0/bytes4/slotState[3] at 8;
// zeroed alignment padding 20..23; six uint64 counters at 24/32/40/48/56/64.
static_assert(sizeof(SharedBridgeStatsV1) == 72, "bridge stats export stays 72 bytes");
static_assert(offsetof(SharedBridgeStatsV1, version) == 0, "stats.version at 0");
static_assert(offsetof(SharedBridgeStatsV1, structBytes) == 4, "stats.structBytes at 4");
static_assert(offsetof(SharedBridgeStatsV1, slotState) == 8, "stats.slotState at 8");
static_assert(offsetof(SharedBridgeStatsV1, slotState) + 3 * sizeof(uint32_t) == 20,
              "stats.slotState occupies 8..19, padding 20..23");
static_assert(offsetof(SharedBridgeStatsV1, initCount) == 24, "stats.initCount at 24");
static_assert(offsetof(SharedBridgeStatsV1, prepareCount) == 32, "stats.prepareCount at 32");
static_assert(offsetof(SharedBridgeStatsV1, commitCount) == 40, "stats.commitCount at 40");
static_assert(offsetof(SharedBridgeStatsV1, dropCount) == 48, "stats.dropCount at 48");
static_assert(offsetof(SharedBridgeStatsV1, drainCount) == 56, "stats.drainCount at 56");
static_assert(offsetof(SharedBridgeStatsV1, quarantineCount) == 64, "stats.quarantineCount at 64");

// ============================================================================
// SharedCaptureIdentityV1 (192 B, Pack8): the frozen capture identity handed to
// PREPARE. Original capture identity fields are frame/pose/epoch/originalGen/
// poseBegin/poseEnd/captureBegin/plans/yaw. PREPARE requires
// captureEndQpc == 0; COMMIT supplies the completion captureEndQpc exactly
// once and EVERY other field/bit pattern must equal the stored PREPARE image.
// Explicit reserved at 36 must be zero. No SDK/COM/delegate/array/caller
// pointer is retained in this layout.
// ============================================================================
#pragma pack(push, 8)
struct SharedCaptureIdentityV1 {
    uint32_t structBytes;        // 0  == 192
    uint32_t version;            // 4  == 1
    uint64_t frameSerial;        // 8  fresh nonzero reserved serial
    uint64_t poseSerial;         // 16 exact captured PoseSample.serial
    uint64_t epoch;              // 24 ring epoch captured by the owner
    uint32_t generation;         // 32 original owner generation (nonzero)
    uint32_t reserved;           // 36 must be 0
    int64_t poseSampleBeginQpc;  // 40
    int64_t poseSampleEndQpc;    // 48
    int64_t captureBeginQpc;     // 56
    int64_t captureEndQpc;       // 64  0 at PREPARE; set ONCE at COMMIT
    double cameraYawDeg;         // 72  finite
    ProjectionPlanV1 leftPlan;   // 80
    ProjectionPlanV1 rightPlan;  // 136
};
static_assert(sizeof(SharedCaptureIdentityV1) == 192, "identity must be exactly 192 bytes");
static_assert(offsetof(SharedCaptureIdentityV1, structBytes) == 0, "id.structBytes at 0");
static_assert(offsetof(SharedCaptureIdentityV1, version) == 4, "id.version at 4");
static_assert(offsetof(SharedCaptureIdentityV1, frameSerial) == 8, "id.frame at 8");
static_assert(offsetof(SharedCaptureIdentityV1, poseSerial) == 16, "id.pose at 16");
static_assert(offsetof(SharedCaptureIdentityV1, epoch) == 24, "id.epoch at 24");
static_assert(offsetof(SharedCaptureIdentityV1, generation) == 32, "id.generation at 32");
static_assert(offsetof(SharedCaptureIdentityV1, reserved) == 36, "id.reserved at 36");
static_assert(offsetof(SharedCaptureIdentityV1, poseSampleBeginQpc) == 40, "id.poseBegin at 40");
static_assert(offsetof(SharedCaptureIdentityV1, poseSampleEndQpc) == 48, "id.poseEnd at 48");
static_assert(offsetof(SharedCaptureIdentityV1, captureBeginQpc) == 56, "id.captureBegin at 56");
static_assert(offsetof(SharedCaptureIdentityV1, captureEndQpc) == 64, "id.captureEnd at 64");
static_assert(offsetof(SharedCaptureIdentityV1, cameraYawDeg) == 72, "id.yaw at 72");
static_assert(offsetof(SharedCaptureIdentityV1, leftPlan) == 80, "id.leftPlan at 80");
static_assert(offsetof(SharedCaptureIdentityV1, rightPlan) == 136, "id.rightPlan at 136");

// SharedStopStateV1 (32 B, Pack8): purely cached native state; no D3D/COM/wait
// is ever executed to answer it, and no flag is inferred from elapsed time.
struct SharedStopStateV1 {
    uint32_t structBytes;      // 0  == 32
    uint32_t version;          // 4  == 1
    uint32_t flags;            // 8  SharedStopFlagV1 bits
    uint32_t pendingSlots;     // 12 shared-mode ring slots not yet FREE
    uint64_t boundEpoch;       // 16 0 while unbound
    uint32_t boundGeneration;  // 24 0 while unbound (uint32: bridge generation)
    uint32_t reserved;         // 28 must be 0
};
static_assert(sizeof(SharedStopStateV1) == 32, "stop state must be exactly 32 bytes");
static_assert(offsetof(SharedStopStateV1, structBytes) == 0, "stop.structBytes at 0");
static_assert(offsetof(SharedStopStateV1, version) == 4, "stop.version at 4");
static_assert(offsetof(SharedStopStateV1, flags) == 8, "stop.flags at 8");
static_assert(offsetof(SharedStopStateV1, pendingSlots) == 12, "stop.pendingSlots at 12");
static_assert(offsetof(SharedStopStateV1, boundEpoch) == 16, "stop.boundEpoch at 16");
static_assert(offsetof(SharedStopStateV1, boundGeneration) == 24, "stop.boundGeneration at 24");
static_assert(offsetof(SharedStopStateV1, reserved) == 28, "stop.reserved at 28");
#pragma pack(pop)

// ============================================================================
// Zeroed serializers for native-PRODUCED wire buffers. Every export writes
// through these so implicit padding (ticket tail 60..63, stats 20..23) is
// provably zero without ever reading uninitialized bytes.
// ============================================================================
inline void SharedTicketSerializeZeroedV1(unsigned char (&out)[64], const SharedPreparedPairV1& t)
{
    memset(out, 0, 64);                                   // tail bytes 60..63 stay zero
    memcpy(out, &t, offsetof(SharedPreparedPairV1, format88) + sizeof(uint32_t)); // 0..59 named fields
}

inline void SharedBridgeStatsSerializeZeroedV1(unsigned char (&out)[72], const SharedBridgeStatsV1& s)
{
    memset(out, 0, 72);                                   // padding 20..23 stays zero
    const size_t head = offsetof(SharedBridgeStatsV1, slotState) + 3 * sizeof(uint32_t); // 0..19
    memcpy(out, &s, head);
    static_assert(offsetof(SharedBridgeStatsV1, quarantineCount) + sizeof(uint64_t) == 72,
                  "counters span 24..71");
    memcpy(out + offsetof(SharedBridgeStatsV1, initCount),
           reinterpret_cast<const unsigned char*>(&s) + offsetof(SharedBridgeStatsV1, initCount),
           72 - offsetof(SharedBridgeStatsV1, initCount));
}

// ============================================================================
// Field-by-field comparators. NO whole-struct memcmp: managed Pack8 mirrors
// carry implicit padding that is NOT an identity field. Doubles compare by
// IEEE bit pattern (finite values are required separately by the validators).
// ============================================================================
inline bool SharedDoubleBitsEqual(double a, double b)
{
    uint64_t ba = 0, bb = 0;
    memcpy(&ba, &a, sizeof(ba));
    memcpy(&bb, &b, sizeof(bb));
    return ba == bb;
}

inline bool SharedPlanBitsEqual(const ProjectionPlanV1& a, const ProjectionPlanV1& b)
{
    return a.structBytes == b.structBytes && a.version == b.version &&
           SharedDoubleBitsEqual(a.tanL, b.tanL) && SharedDoubleBitsEqual(a.tanR, b.tanR) &&
           SharedDoubleBitsEqual(a.tanU, b.tanU) && SharedDoubleBitsEqual(a.tanD, b.tanD) &&
           SharedDoubleBitsEqual(a.tx, b.tx) && SharedDoubleBitsEqual(a.ty, b.ty);
}

// Named ticket fields only; the four tail padding bytes are never compared.
inline bool SharedTicketFieldsEqual(const SharedPreparedPairV1& a, const SharedPreparedPairV1& b)
{
    return a.version == b.version && a.structBytes == b.structBytes &&
           a.slotIndex == b.slotIndex && a.generation == b.generation &&
           a.epoch == b.epoch && a.ticket == b.ticket &&
           a.legacyLeft == b.legacyLeft && a.legacyRight == b.legacyRight &&
           a.width == b.width && a.height == b.height && a.format88 == b.format88;
}

// Full named-field identity equality (every integer + every double bit
// pattern). Used by COMMIT to prove the caller resubmits the stored PREPARE
// image except for the single completion field.
inline bool SharedIdentityFieldsEqual(const SharedCaptureIdentityV1& a, const SharedCaptureIdentityV1& b)
{
    return a.structBytes == b.structBytes && a.version == b.version &&
           a.frameSerial == b.frameSerial && a.poseSerial == b.poseSerial &&
           a.epoch == b.epoch && a.generation == b.generation && a.reserved == b.reserved &&
           a.poseSampleBeginQpc == b.poseSampleBeginQpc && a.poseSampleEndQpc == b.poseSampleEndQpc &&
           a.captureBeginQpc == b.captureBeginQpc && a.captureEndQpc == b.captureEndQpc &&
           SharedDoubleBitsEqual(a.cameraYawDeg, b.cameraYawDeg) &&
           SharedPlanBitsEqual(a.leftPlan, b.leftPlan) &&
           SharedPlanBitsEqual(a.rightPlan, b.rightPlan);
}

// COMMIT comparison: everything except captureEndQpc must be bit-identical to
// the stored PREPARE identity (which itself has captureEndQpc == 0).
inline bool SharedIdentityEqualExceptCompletion(const SharedCaptureIdentityV1& stored,
                                                 const SharedCaptureIdentityV1& commit)
{
    SharedCaptureIdentityV1 a = stored;
    SharedCaptureIdentityV1 b = commit;
    a.captureEndQpc = 0;
    b.captureEndQpc = 0;
    return SharedIdentityFieldsEqual(a, b);
}

} // namespace VRSIM

#endif // VRSIM_SHARED_STEREO_WIRE_H
