// VRSIMPlugin.SharedGPU.cs — HIGHRES-H2SH-CARD3W: hosted wire / P-Invoke
// bindings ONLY. Nothing here is wired into the producer loop, the UI, or any
// automatic refresh; no shared capability is advertised. The native exports
// behind these signatures belong to the frozen CARD1 wire contract
// (SharedStereoWire.h / VRSIMNative.h) and the native side is owned by a
// separate card.
//
// Hard rules re-asserted here:
//  - legacyLeft/legacyRight in the ticket are shared HANDLE NUMBERS. Managed
//    code must NEVER CloseHandle/DuplicateHandle them.
//  - Padding bytes are serialized zero by native and NEVER compared as part of
//    ticket identity (named fields only).
//  - Full COMMIT identity comparison (every other field bit-identical to the
//    stored PREPARE image, captureEndQpc supplied exactly once) is done by
//    native — the wrappers here never rewrite pose/epoch/identity fields.
//  - Missing DLL/exports are never swallowed into a fallback: the managed
//    loader exception propagates honestly. Only local layout/shape problems
//    are converted to a BadArgument rc, and never a fake success.
using System;
using System.Runtime.InteropServices;

#if VRSIM_QA_MIXED_SOURCE && !(VRSIM_NATIVE_QA && NC02_XR_QA && VRSIM_SHARED_3072 && VRSIM_SOURCE_4096_QA)
#error VRSIM_QA_MIXED_SOURCE requires all four isolated QA symbols
#endif
#if VRSIM_QA_D9_BEGIN_POLL && !(VRSIM_NATIVE_QA && NC02_XR_QA && VRSIM_QA_MIXED_SOURCE && VRSIM_SHARED_3072 && VRSIM_SOURCE_4096_QA)
#error VRSIM_QA_D9_BEGIN_POLL requires VRSIM_NATIVE_QA + NC02_XR_QA + VRSIM_QA_MIXED_SOURCE + VRSIM_SHARED_3072 + VRSIM_SOURCE_4096_QA
#endif
#if VRSIM_QA_OUTPUT_4096 && !(VRSIM_NATIVE_QA && NC02_XR_QA && VRSIM_SHARED_3072 && VRSIM_SOURCE_4096_QA && VRSIM_QA_MIXED_SOURCE && VRSIM_QA_D9_BEGIN_POLL)
#error VRSIM_QA_OUTPUT_4096 requires VRSIM_NATIVE_QA + NC02_XR_QA + VRSIM_SHARED_3072 + VRSIM_SOURCE_4096_QA + VRSIM_QA_MIXED_SOURCE + VRSIM_QA_D9_BEGIN_POLL
#endif
#if VRSIM_SHARED_3072 && (VRSIM_SHARED_2736 || VRSIM_SHARED_2560 || VRSIM_SHARED_2048)
#error VRSIM_SHARED_3072 is mutually exclusive with other shared size variants
#endif
#if VRSIM_SHARED_2736 && VRSIM_SHARED_2560
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2560 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_2736 && VRSIM_SHARED_2048
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_2560 && VRSIM_SHARED_2048
#error VRSIM_SHARED_2560 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif

namespace VRSIM
{
    public sealed partial class VRSIMPlugin
    {
        // ---- frozen wire constants (SharedStereoWire.h) ----
        private const uint SharedGpuWireVersionV1 = 1u;
        private const uint SharedGpuTicketBytesV1 = 64u;
        private const uint SharedGpuIdentityBytesV1 = 192u;
        private const uint SharedGpuStopBytesV1 = 32u;
        private const uint SharedGpuStatsBytesV1 = 72u;
#if VRSIM_QA_MIXED_SOURCE
        // QA-only mixed source: the ticket edge carries the SOURCE square (4096);
        // the OUTPUT swap/projection edge stays 3072 and is exported separately as
        // SharedGpuOutputEdgeV1. Wire sizes/offsets are unchanged.
        private const uint SharedGpuTicketEdgeV1 = 4096u;
#elif VRSIM_SHARED_3072
        // Isolated 3072 compile variant: the ticket edge follows the locked
        // 3072x3072 shared-bridge square; wire sizes/offsets are unchanged.
        private const uint SharedGpuTicketEdgeV1 = 3072u;
#elif VRSIM_SHARED_2736
        // Isolated 2736 compile variant: the ticket edge follows the locked
        // 2736x2736 shared-bridge square; wire sizes/offsets are unchanged.
        private const uint SharedGpuTicketEdgeV1 = 2736u;
#elif VRSIM_SHARED_2560
        // Isolated 2560 compile variant: the ticket edge follows the locked
        // 2560x2560 shared-bridge square; wire sizes/offsets are unchanged.
        private const uint SharedGpuTicketEdgeV1 = 2560u;
#elif VRSIM_SHARED_2048
        private const uint SharedGpuTicketEdgeV1 = 2048u;
#else
        private const uint SharedGpuTicketEdgeV1 = 1536u;
#endif
        // OUTPUT edge consumed by Main monitor/entry gates. This constant is QA-only:
        // it exists ONLY when VRSIM_QA_MIXED_SOURCE is defined so the default build
        // keeps an identical byte footprint to the baseline.
#if VRSIM_QA_MIXED_SOURCE
#if VRSIM_QA_OUTPUT_4096
        // Current 4K branch: OUTPUT swap/projection edge is 4096; the SOURCE ticket edge
        // stays 4096 via SharedGpuTicketEdgeV1 above. Wire sizes/offsets unchanged.
        private const uint SharedGpuOutputEdgeV1 = 4096u;
#else
        // Old 3K branch: OUTPUT swap/projection edge stays 3072. Wire sizes/offsets unchanged.
        private const uint SharedGpuOutputEdgeV1 = 3072u;
#endif
#endif
        private const uint SharedGpuTicketFormat88V1 = 88u;
        private const int SharedGpuSlotCountV1 = 3;

        // Shared export return codes (VRSIM::SharedExportResultV1). Anything
        // other than Completed is NOT a success.
        internal enum SharedGpuResultV1 : int
        {
            Completed = 0,
            Pending = 1,
            BadArgument = -1,
            Unavailable = -2,
            Stale = -3,
            Quarantined = -4,
            Error = -5
        }

        // ---- Pack8 blittable mirrors of the frozen native wire ----

        // ticket64: version0/structBytes4/slotIndex8/generation12;
        // epoch16/ticket24/legacyLeft32/legacyRight40; width48/height52/
        // format88@56; explicit reservedPadding@60 (native serializes zero;
        // never part of ticket identity).
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct SharedGpuTicketV1
        {
            public uint version;
            public uint structBytes;
            public uint slotIndex;
            public uint generation;
            public ulong epoch;
            public ulong ticket;
            public ulong legacyLeft;   // shared handle NUMBER — never close/duplicate
            public ulong legacyRight;  // shared handle NUMBER — never close/duplicate
            public uint width;
            public uint height;
            public uint format88;
            public uint reservedPadding; // must read back 0; never identity
        }

        // identity192: structBytes0/version4; frameSerial8/poseSerial16/
        // epoch24; generation32/reserved36; poseBegin40/poseEnd48/
        // captureBegin56/captureEnd64; yaw72; leftPlan80/rightPlan136 (the
        // original frozen RawProjectionPlanV1, reused — not re-declared).
        // captureEndQpc MUST be 0 at PREPARE and is supplied exactly ONCE at
        // COMMIT; every other field/bit pattern stays unchanged.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct SharedGpuCaptureIdentityV1
        {
            public uint structBytes;
            public uint version;
            public ulong frameSerial;
            public ulong poseSerial;
            public ulong epoch;
            public uint generation;
            public uint reserved; // must be 0
            public long poseSampleBeginQpc;
            public long poseSampleEndQpc;
            public long captureBeginQpc;
            public long captureEndQpc; // 0 at PREPARE; set once at COMMIT
            public double cameraYawDeg;
            public RawProjectionPlanV1 leftPlan;
            public RawProjectionPlanV1 rightPlan;
        }

        // stop32: structBytes0/version4/flags8/pendingSlots12; boundEpoch16;
        // boundGeneration24/reserved28. flags = cached bits only
        // (1 admission-closed, 2 worker-drained, 4 source-finalized,
        // 8 quarantined) — NEVER inferred from elapsed time.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct SharedGpuStopStateV1
        {
            public uint structBytes;
            public uint version;
            public uint flags;
            public uint pendingSlots;
            public ulong boundEpoch;
            public uint boundGeneration;
            public uint reserved; // must be 0
        }

        // stats72: version0/structBytes4/slotState[3] at 8; explicit zero
        // padding@20; six uint64 counters at 24/32/40/48/56/64.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct SharedGpuBridgeStatsV1
        {
            public uint version;
            public uint structBytes;
            public uint slotState0;
            public uint slotState1;
            public uint slotState2;
            public uint reservedPadding; // must read back 0
            public ulong initCount;
            public ulong prepareCount;
            public ulong commitCount;
            public ulong dropCount;
            public ulong drainCount;
            public ulong quarantineCount;
        }

        // ---- hosted P/Invoke bindings (NativeDllName reused from the main
        // partial: the same VRSIM_NATIVE_QA-conditional DLL name; Cdecl +
        // ExactSpelling per the frozen C ABI) ----

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_PrepareSharedStereoV1(
            ulong leftSurface, ulong rightSurface,
            ref SharedGpuCaptureIdentityV1 identity, uint identityCapacityBytes,
            out SharedGpuTicketV1 outTicket, uint ticketCapacityBytes);

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_CommitSharedStereoV1(
            ref SharedGpuTicketV1 ticket, uint ticketCapacityBytes,
            ref SharedGpuCaptureIdentityV1 identity, uint identityCapacityBytes);

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_DiscardSharedStereoV1(
            ref SharedGpuTicketV1 ticket, uint ticketCapacityBytes);

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_RequestSharedDrainV1();

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_GetSharedStopStateV1(
            out SharedGpuStopStateV1 state, uint capacityBytes);

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_GetSharedBridgeStatsV1(
            out SharedGpuBridgeStatsV1 stats, uint capacityBytes);

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_FinalizeSharedSourceUIV1();

        // ---- pure managed wire-layout validation (Marshal only; touches no
        // native code, no GPU). Covers the four NEW structs AND the two
        // original frozen layouts (Plan56 / Stats120). Cached after first run.
        private static bool? _sharedGpuWireLayoutValid;

        internal static bool SharedGpuWireLayoutValid()
        {
            bool? cached = _sharedGpuWireLayoutValid;
            if (cached.HasValue) return cached.Value;
            bool ok = ComputeSharedGpuWireLayoutValid();
            _sharedGpuWireLayoutValid = ok;
            return ok;
        }

        private static bool ComputeSharedGpuWireLayoutValid()
        {
            // ticket64
            if (Marshal.SizeOf(typeof(SharedGpuTicketV1)) != 64) return false;
            if (OffsetOf<SharedGpuTicketV1>("version") != 0) return false;
            if (OffsetOf<SharedGpuTicketV1>("structBytes") != 4) return false;
            if (OffsetOf<SharedGpuTicketV1>("slotIndex") != 8) return false;
            if (OffsetOf<SharedGpuTicketV1>("generation") != 12) return false;
            if (OffsetOf<SharedGpuTicketV1>("epoch") != 16) return false;
            if (OffsetOf<SharedGpuTicketV1>("ticket") != 24) return false;
            if (OffsetOf<SharedGpuTicketV1>("legacyLeft") != 32) return false;
            if (OffsetOf<SharedGpuTicketV1>("legacyRight") != 40) return false;
            if (OffsetOf<SharedGpuTicketV1>("width") != 48) return false;
            if (OffsetOf<SharedGpuTicketV1>("height") != 52) return false;
            if (OffsetOf<SharedGpuTicketV1>("format88") != 56) return false;
            if (OffsetOf<SharedGpuTicketV1>("reservedPadding") != 60) return false;

            // identity192
            if (Marshal.SizeOf(typeof(SharedGpuCaptureIdentityV1)) != 192) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("structBytes") != 0) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("version") != 4) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("frameSerial") != 8) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("poseSerial") != 16) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("epoch") != 24) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("generation") != 32) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("reserved") != 36) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("poseSampleBeginQpc") != 40) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("poseSampleEndQpc") != 48) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("captureBeginQpc") != 56) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("captureEndQpc") != 64) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("cameraYawDeg") != 72) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("leftPlan") != 80) return false;
            if (OffsetOf<SharedGpuCaptureIdentityV1>("rightPlan") != 136) return false;

            // stop32
            if (Marshal.SizeOf(typeof(SharedGpuStopStateV1)) != 32) return false;
            if (OffsetOf<SharedGpuStopStateV1>("structBytes") != 0) return false;
            if (OffsetOf<SharedGpuStopStateV1>("version") != 4) return false;
            if (OffsetOf<SharedGpuStopStateV1>("flags") != 8) return false;
            if (OffsetOf<SharedGpuStopStateV1>("pendingSlots") != 12) return false;
            if (OffsetOf<SharedGpuStopStateV1>("boundEpoch") != 16) return false;
            if (OffsetOf<SharedGpuStopStateV1>("boundGeneration") != 24) return false;
            if (OffsetOf<SharedGpuStopStateV1>("reserved") != 28) return false;

            // stats72
            if (Marshal.SizeOf(typeof(SharedGpuBridgeStatsV1)) != 72) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("version") != 0) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("structBytes") != 4) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("slotState0") != 8) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("slotState1") != 12) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("slotState2") != 16) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("reservedPadding") != 20) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("initCount") != 24) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("prepareCount") != 32) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("commitCount") != 40) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("dropCount") != 48) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("drainCount") != 56) return false;
            if (OffsetOf<SharedGpuBridgeStatsV1>("quarantineCount") != 64) return false;

            // original frozen layouts, re-checked unchanged
            if (Marshal.SizeOf(typeof(RawProjectionPlanV1)) != 56) return false;
            if (OffsetOf<RawProjectionPlanV1>("structBytes") != 0) return false;
            if (OffsetOf<RawProjectionPlanV1>("version") != 4) return false;
            if (OffsetOf<RawProjectionPlanV1>("tanL") != 8) return false;
            if (OffsetOf<RawProjectionPlanV1>("ty") != 48) return false;
            if (Marshal.SizeOf(typeof(GpuPipelineStatsV1)) != 120) return false;
            if (OffsetOf<GpuPipelineStatsV1>("structBytes") != 0) return false;
            if (OffsetOf<GpuPipelineStatsV1>("version") != 4) return false;
            if (OffsetOf<GpuPipelineStatsV1>("lastGpuDispatchQpcTicks") != 104) return false;
            if (OffsetOf<GpuPipelineStatsV1>("sessionEpoch") != 112) return false;
            return true;
        }

        private static int OffsetOf<T>(string fieldName)
        {
            return Marshal.OffsetOf(typeof(T), fieldName).ToInt32();
        }

        // Named-field ticket shape only (padding never identity): header must
        // read back exactly, slot in range, and the FIXED-route protocol shape
        // strictly: width==1536, height==1536, format88==88, with
        // generation/epoch/ticket all nonzero. Not relaxed for hypothetical
        // future configurations.
        private static bool SharedGpuTicketShapeValid(SharedGpuTicketV1 t)
        {
            if (t.version != SharedGpuWireVersionV1) return false;
            if (t.structBytes != SharedGpuTicketBytesV1) return false;
            if (t.reservedPadding != 0u) return false;
            if (t.slotIndex >= (uint)SharedGpuSlotCountV1) return false;
            if (t.width != SharedGpuTicketEdgeV1) return false;
            if (t.height != SharedGpuTicketEdgeV1) return false;
            if (t.format88 != SharedGpuTicketFormat88V1) return false;
            if (t.generation == 0u) return false;
            if (t.epoch == 0ul) return false;
            if (t.ticket == 0ul) return false;
            return true;
        }

        private static bool SharedGpuStopShapeValid(SharedGpuStopStateV1 s)
        {
            if (s.structBytes != SharedGpuStopBytesV1) return false;
            if (s.version != SharedGpuWireVersionV1) return false;
            if (s.reserved != 0u) return false;
            if ((s.flags & ~15u) != 0u) return false; // only bits 1/2/4/8 defined
            // bound epoch/generation are reported together: both zero or both
            // nonzero — a half-bound read is not honest cached state.
            if ((s.boundEpoch == 0ul) != (s.boundGeneration == 0u)) return false;
            return true;
        }

        private static bool SharedGpuStatsShapeValid(SharedGpuBridgeStatsV1 st)
        {
            if (st.version != SharedGpuWireVersionV1) return false;
            if (st.structBytes != SharedGpuStatsBytesV1) return false;
            if (st.reservedPadding != 0u) return false;
            return true;
        }

        // ---- internal wrappers for the FUTURE producer/owner card ----
        // Fixed capacities, actual native rc preserved. On a local layout
        // failure the call is rejected with BadArgument WITHOUT touching
        // native; a native rc==0 with an invalid read-back shape is likewise
        // downgraded to BadArgument — never a fake success. Missing DLL/export
        // exceptions are NOT swallowed into any fallback.

        // Prepare: identity must carry captureEndQpc == 0 (native enforces).
        // The granted ticket is shape-checked only after rc==0; on a check
        // failure the ticket is STILL returned in `out` so the caller can
        // discard it exactly — the wrapper never silently clears it.
        internal int PrepareSharedGpu(
            ulong leftSurface, ulong rightSurface,
            ref SharedGpuCaptureIdentityV1 identity,
            out SharedGpuTicketV1 ticket)
        {
            ticket = default(SharedGpuTicketV1);
            if (!SharedGpuWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            SharedGpuTicketV1 granted;
            int rc = VRSIM_PrepareSharedStereoV1(
                leftSurface, rightSurface,
                ref identity, SharedGpuIdentityBytesV1,
                out granted, SharedGpuTicketBytesV1);
            ticket = granted;
            if (rc == (int)SharedGpuResultV1.Completed && !SharedGpuTicketShapeValid(granted))
                return (int)SharedGpuResultV1.BadArgument;
            return rc;
        }

        // Commit: full identity comparison is native's job. The wrapper passes
        // the caller's ticket/identity through unchanged (including double bit
        // patterns) and only forwards the rc.
        internal int CommitSharedGpu(
            ref SharedGpuTicketV1 ticket, ref SharedGpuCaptureIdentityV1 identity)
        {
            if (!SharedGpuWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            return VRSIM_CommitSharedStereoV1(
                ref ticket, SharedGpuTicketBytesV1,
                ref identity, SharedGpuIdentityBytesV1);
        }

        internal int DiscardSharedGpu(ref SharedGpuTicketV1 ticket)
        {
            if (!SharedGpuWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            return VRSIM_DiscardSharedStereoV1(ref ticket, SharedGpuTicketBytesV1);
        }

        internal int RequestSharedGpuDrain()
        {
            if (!SharedGpuWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            return VRSIM_RequestSharedDrainV1();
        }

        internal int ReadSharedGpuStop(out SharedGpuStopStateV1 state)
        {
            state = default(SharedGpuStopStateV1);
            if (!SharedGpuWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            SharedGpuStopStateV1 read;
            int rc = VRSIM_GetSharedStopStateV1(out read, SharedGpuStopBytesV1);
            state = read;
            if (rc == (int)SharedGpuResultV1.Completed && !SharedGpuStopShapeValid(read))
                return (int)SharedGpuResultV1.BadArgument;
            return rc;
        }

        internal int ReadSharedGpuBridgeStats(out SharedGpuBridgeStatsV1 stats)
        {
            stats = default(SharedGpuBridgeStatsV1);
            if (!SharedGpuWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            SharedGpuBridgeStatsV1 read;
            int rc = VRSIM_GetSharedBridgeStatsV1(out read, SharedGpuStatsBytesV1);
            stats = read;
            if (rc == (int)SharedGpuResultV1.Completed && !SharedGpuStatsShapeValid(read))
                return (int)SharedGpuResultV1.BadArgument;
            return rc;
        }

        internal int FinalizeSharedGpuSourceUI()
        {
            if (!SharedGpuWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            return VRSIM_FinalizeSharedSourceUIV1();
        }

        // Card3A thin shared-capture cache-clear wrapper. Reuses the existing private
        // ClearPreviewForRawEnqueue() (same partial class) to drop any stale old-mode monitor
        // preview bytes/numbers at a fresh shared Start — WITHOUT producing replacement bytes and
        // WITHOUT calling StorePreview or fetching CPU pixels. So TryGetPreview can never return a
        // stale old-mode pixel set while the shared route is wired. The original telemetry
        // (_previewEpoch / _previewStoreCount) stays untouched by the reused helper.
        internal void ClearSharedGpuPreviewCache()
        {
            ClearPreviewForRawEnqueue();
        }

#if VRSIM_QA_D9_BEGIN_POLL
        // ==== Card2a QA-only managed Begin/Poll/Cancel binding ====
        // Compiled ONLY under VRSIM_QA_D9_BEGIN_POLL (guarded fail-closed above:
        // the define requires all of VRSIM_NATIVE_QA, NC02_XR_QA,
        // VRSIM_QA_MIXED_SOURCE, VRSIM_SHARED_3072 and VRSIM_SOURCE_4096_QA).
        // NOTHING here is wired into the producer, scheduler, stop or any
        // automatic path; the frozen 56/120/192/64/32/72 layouts, existing
        // imports and existing wrappers are untouched and the default/OFF
        // build has the identical byte footprint.

        // ---- QA D9 wire constants ----
        private const uint QaSharedWireVersionV1 = 1u;
        private const uint QaSharedTokenBytesV1 = 32u;
        private const uint QaSharedBeginResultBytesV1 = 80u;
        private const uint QaSharedPollResultBytesV1 = 56u;

        // SharedGpuExportStatusV1 (result.status, uint32). Pending (3) is NOT a
        // completion; every other value is owned by the future classifier.
        internal enum QaSharedExportStatusV1 : uint
        {
            None = 0u,
            Rejected = 1u,
            PreWorkRolledBack = 2u,
            Pending = 3u,
            ImmediateGranted = 4u,
            CompletedGranted = 5u,
            CompletedNoGrant = 6u,
            FailedDrained = 7u,
            QuarantinedRetained = 8u
        }

        // SharedGpuLedgerStateV1 (result.ledgerStateAfter, uint32).
        internal enum QaSharedLedgerStateV1 : uint
        {
            Empty = 0u,
            Preparing = 1u,
            Quarantined = 2u
        }

        // token32: version0/structBytes4/slotIndex8/generation12/epoch16/nonce24.
        // A Begin token is valid ONLY for status Pending; zero for every other
        // Begin status. Poll consumes the saved original token and returns none.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct QaSharedTokenV1
        {
            public uint version;
            public uint structBytes;
            public uint slotIndex;
            public uint generation;
            public ulong epoch;
            public ulong nonce;
        }

        // beginResult80: version0/structBytes4/status8/bridgeHresult12(INT)/
        // copyExecMask16/copyOkMask20/faultConsumed24/ledgerStateAfter28/
        // firstFailureHr32(INT)/lastIssueHr36(INT)/lastGetDataHr40(INT)/
        // getDataCallsThisCall44; nested token32 @48.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct QaSharedBeginResultV1
        {
            public uint version;
            public uint structBytes;
            public uint status;
            public int bridgeHresult;
            public uint copyExecMask;
            public uint copyOkMask;
            public uint faultConsumed;
            public uint ledgerStateAfter;
            public int firstFailureHr;
            public int lastIssueHr;
            public int lastGetDataHr;
            public uint getDataCallsThisCall;
            public QaSharedTokenV1 token;
        }

        // pollResult56: version0/structBytes4/status8/bridgeHresult12(INT)/
        // lastGetDataHr16(INT)/firstFailureHr20(INT)/getDataCallsThisCall24/
        // pairOkMask28/elapsedMs32/getDataCallsTotal40/ticketGranted48/
        // ledgerStateAfter52. No returned token: the caller saves the original.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct QaSharedPollResultV1
        {
            public uint version;
            public uint structBytes;
            public uint status;
            public int bridgeHresult;
            public int lastGetDataHr;
            public int firstFailureHr;
            public uint getDataCallsThisCall;
            public uint pairOkMask;
            public ulong elapsedMs;
            public ulong getDataCallsTotal;
            public uint ticketGranted;
            public uint ledgerStateAfter;
        }

        // ---- QA D9 hosted P/Invoke bindings (Cdecl + ExactSpelling, same
        // NativeDllName as the frozen bindings; void* buffers map to blittable
        // ref/out structs with fixed capacities — no bool marshalling) ----

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_QaBeginSharedStereoV1(
            ulong leftSurface, ulong rightSurface,
            ref SharedGpuCaptureIdentityV1 identity192, uint identityBytes,
            out QaSharedBeginResultV1 result80, uint resultBytes,
            out SharedGpuTicketV1 ticket64, uint ticketBytes);

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_QaPollSharedStereoV1(
            ref QaSharedTokenV1 token32, uint tokenBytes,
            out QaSharedPollResultV1 result56, uint resultBytes,
            out SharedGpuTicketV1 ticket64, uint ticketBytes);

        [DllImport(NativeDllName, CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int VRSIM_QaCancelSharedStereoV1(
            ref QaSharedTokenV1 token32, uint tokenBytes,
            out QaSharedBeginResultV1 result80, uint resultBytes);

        // ---- pure managed QA D9 layout validation (Marshal/type metadata
        // only; no native, no SDK) — every size, every named offset, the
        // signed (int32) HRESULT fields, the nested token field type, PLUS the
        // unchanged existing frozen-layout validator result. ----
        private static bool? _qaSharedWireLayoutValid;

        internal static bool QaSharedWireLayoutValid()
        {
            bool? cached = _qaSharedWireLayoutValid;
            if (cached.HasValue) return cached.Value;
            bool ok = ComputeQaSharedWireLayoutValid();
            _qaSharedWireLayoutValid = ok;
            return ok;
        }

        private static bool ComputeQaSharedWireLayoutValid()
        {
            // the frozen baseline layouts must still validate unchanged
            if (!SharedGpuWireLayoutValid()) return false;

            // token32
            if (Marshal.SizeOf(typeof(QaSharedTokenV1)) != 32) return false;
            if (OffsetOf<QaSharedTokenV1>("version") != 0) return false;
            if (OffsetOf<QaSharedTokenV1>("structBytes") != 4) return false;
            if (OffsetOf<QaSharedTokenV1>("slotIndex") != 8) return false;
            if (OffsetOf<QaSharedTokenV1>("generation") != 12) return false;
            if (OffsetOf<QaSharedTokenV1>("epoch") != 16) return false;
            if (OffsetOf<QaSharedTokenV1>("nonce") != 24) return false;

            // beginResult80
            if (Marshal.SizeOf(typeof(QaSharedBeginResultV1)) != 80) return false;
            if (OffsetOf<QaSharedBeginResultV1>("version") != 0) return false;
            if (OffsetOf<QaSharedBeginResultV1>("structBytes") != 4) return false;
            if (OffsetOf<QaSharedBeginResultV1>("status") != 8) return false;
            if (OffsetOf<QaSharedBeginResultV1>("bridgeHresult") != 12) return false;
            if (OffsetOf<QaSharedBeginResultV1>("copyExecMask") != 16) return false;
            if (OffsetOf<QaSharedBeginResultV1>("copyOkMask") != 20) return false;
            if (OffsetOf<QaSharedBeginResultV1>("faultConsumed") != 24) return false;
            if (OffsetOf<QaSharedBeginResultV1>("ledgerStateAfter") != 28) return false;
            if (OffsetOf<QaSharedBeginResultV1>("firstFailureHr") != 32) return false;
            if (OffsetOf<QaSharedBeginResultV1>("lastIssueHr") != 36) return false;
            if (OffsetOf<QaSharedBeginResultV1>("lastGetDataHr") != 40) return false;
            if (OffsetOf<QaSharedBeginResultV1>("getDataCallsThisCall") != 44) return false;
            if (OffsetOf<QaSharedBeginResultV1>("token") != 48) return false;
            if (FieldType<QaSharedBeginResultV1>("version") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("structBytes") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("status") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("bridgeHresult") != typeof(int)) return false;
            if (FieldType<QaSharedBeginResultV1>("copyExecMask") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("copyOkMask") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("faultConsumed") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("ledgerStateAfter") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("firstFailureHr") != typeof(int)) return false;
            if (FieldType<QaSharedBeginResultV1>("lastIssueHr") != typeof(int)) return false;
            if (FieldType<QaSharedBeginResultV1>("lastGetDataHr") != typeof(int)) return false;
            if (FieldType<QaSharedBeginResultV1>("getDataCallsThisCall") != typeof(uint)) return false;
            if (FieldType<QaSharedBeginResultV1>("token") != typeof(QaSharedTokenV1)) return false;

            // pollResult56
            if (Marshal.SizeOf(typeof(QaSharedPollResultV1)) != 56) return false;
            if (OffsetOf<QaSharedPollResultV1>("version") != 0) return false;
            if (OffsetOf<QaSharedPollResultV1>("structBytes") != 4) return false;
            if (OffsetOf<QaSharedPollResultV1>("status") != 8) return false;
            if (OffsetOf<QaSharedPollResultV1>("bridgeHresult") != 12) return false;
            if (OffsetOf<QaSharedPollResultV1>("lastGetDataHr") != 16) return false;
            if (OffsetOf<QaSharedPollResultV1>("firstFailureHr") != 20) return false;
            if (OffsetOf<QaSharedPollResultV1>("getDataCallsThisCall") != 24) return false;
            if (OffsetOf<QaSharedPollResultV1>("pairOkMask") != 28) return false;
            if (OffsetOf<QaSharedPollResultV1>("elapsedMs") != 32) return false;
            if (OffsetOf<QaSharedPollResultV1>("getDataCallsTotal") != 40) return false;
            if (OffsetOf<QaSharedPollResultV1>("ticketGranted") != 48) return false;
            if (OffsetOf<QaSharedPollResultV1>("ledgerStateAfter") != 52) return false;
            if (FieldType<QaSharedPollResultV1>("version") != typeof(uint)) return false;
            if (FieldType<QaSharedPollResultV1>("structBytes") != typeof(uint)) return false;
            if (FieldType<QaSharedPollResultV1>("status") != typeof(uint)) return false;
            if (FieldType<QaSharedPollResultV1>("bridgeHresult") != typeof(int)) return false;
            if (FieldType<QaSharedPollResultV1>("lastGetDataHr") != typeof(int)) return false;
            if (FieldType<QaSharedPollResultV1>("firstFailureHr") != typeof(int)) return false;
            if (FieldType<QaSharedPollResultV1>("getDataCallsThisCall") != typeof(uint)) return false;
            if (FieldType<QaSharedPollResultV1>("pairOkMask") != typeof(uint)) return false;
            if (FieldType<QaSharedPollResultV1>("elapsedMs") != typeof(ulong)) return false;
            if (FieldType<QaSharedPollResultV1>("getDataCallsTotal") != typeof(ulong)) return false;
            if (FieldType<QaSharedPollResultV1>("ticketGranted") != typeof(uint)) return false;
            if (FieldType<QaSharedPollResultV1>("ledgerStateAfter") != typeof(uint)) return false;

            // token field types (uint = uint32, ulong = uint64)
            if (FieldType<QaSharedTokenV1>("version") != typeof(uint)) return false;
            if (FieldType<QaSharedTokenV1>("structBytes") != typeof(uint)) return false;
            if (FieldType<QaSharedTokenV1>("slotIndex") != typeof(uint)) return false;
            if (FieldType<QaSharedTokenV1>("generation") != typeof(uint)) return false;
            if (FieldType<QaSharedTokenV1>("epoch") != typeof(ulong)) return false;
            if (FieldType<QaSharedTokenV1>("nonce") != typeof(ulong)) return false;
            return true;
        }

        private static Type FieldType<T>(string fieldName)
        {
            System.Reflection.FieldInfo field = typeof(T).GetField(fieldName);
            if (field == null) return null;
            return field.FieldType;
        }

        // ---- QA D9 pure readback-shape checks. These report an independent
        // bool ONLY; the wrapper NEVER rewrites the actual native rc, and
        // malformed actual outputs are preserved for exact later cleanup.
        // Nothing here classifies rc/status semantics: rc==1 alone is not
        // treated as genuine Pending (a pre-claim busy return has status None
        // and no token) and Cancel rc==1 is not converted to drain/completion.

        // Exact valid pending token: version1/bytes32/slot<3/gen>0/epoch>0/nonce>0.
        private static bool QaSharedPendingTokenShapeValid(QaSharedTokenV1 token)
        {
            if (token.version != QaSharedWireVersionV1) return false;
            if (token.structBytes != QaSharedTokenBytesV1) return false;
            if (token.slotIndex >= 3u) return false;
            if (token.generation == 0u) return false;
            if (token.epoch == 0ul) return false;
            if (token.nonce == 0ul) return false;
            return true;
        }

        // All-zero token (required for every non-Pending Begin status).
        private static bool QaSharedZeroTokenShapeValid(QaSharedTokenV1 token)
        {
            if (token.version != 0u) return false;
            if (token.structBytes != 0u) return false;
            if (token.slotIndex != 0u) return false;
            if (token.generation != 0u) return false;
            if (token.epoch != 0ul) return false;
            if (token.nonce != 0ul) return false;
            return true;
        }

        private static bool QaSharedBeginResultShapeValid(
            QaSharedBeginResultV1 r, SharedGpuTicketV1 ticket, bool ticketPresent)
        {
            if (r.version != QaSharedWireVersionV1) return false;
            if (r.structBytes != QaSharedBeginResultBytesV1) return false;
            if (r.status > (uint)QaSharedExportStatusV1.QuarantinedRetained) return false;
            if (r.ledgerStateAfter > (uint)QaSharedLedgerStateV1.Quarantined) return false;
            if (r.getDataCallsThisCall > 1u) return false;
            // copyExecMask/copyOkMask live within the two-eye bit space (bits 0/1).
            if ((r.copyExecMask & ~3u) != 0u) return false;
            if ((r.copyOkMask & ~3u) != 0u) return false;
            if (r.status == (uint)QaSharedExportStatusV1.Pending)
            {
                // A Begin token is valid ONLY for Pending, exact fields required.
                if (!QaSharedPendingTokenShapeValid(r.token)) return false;
            }
            else
            {
                if (!QaSharedZeroTokenShapeValid(r.token)) return false;
            }
            if (ticketPresent)
            {
                // Existing frozen ticket shape reused unchanged; malformed
                // actual tickets are preserved by the wrapper regardless.
                if (!SharedGpuTicketShapeValid(ticket)) return false;
            }
            return true;
        }

        private static bool QaSharedPollResultShapeValid(
            QaSharedPollResultV1 r, SharedGpuTicketV1 ticket, bool ticketPresent)
        {
            if (r.version != QaSharedWireVersionV1) return false;
            if (r.structBytes != QaSharedPollResultBytesV1) return false;
            if (r.status > (uint)QaSharedExportStatusV1.QuarantinedRetained) return false;
            if (r.ledgerStateAfter > (uint)QaSharedLedgerStateV1.Quarantined) return false;
            if (r.getDataCallsThisCall > 1u) return false;
            if ((r.pairOkMask & ~3u) != 0u) return false;
            if (r.ticketGranted > 1u) return false;
            if (r.ticketGranted == 1u && !ticketPresent) return false;
            if (ticketPresent && !SharedGpuTicketShapeValid(ticket)) return false;
            return true;
        }

        private static bool QaSharedTokenIsPresent(QaSharedTokenV1 t)
        {
            return t.version != 0u || t.structBytes != 0u || t.slotIndex != 0u
                || t.generation != 0u || t.epoch != 0ul || t.nonce != 0ul;
        }

        private static bool QaSharedTokenEquals(QaSharedTokenV1 a, QaSharedTokenV1 b)
        {
            return a.version == b.version && a.structBytes == b.structBytes
                && a.slotIndex == b.slotIndex && a.generation == b.generation
                && a.epoch == b.epoch && a.nonce == b.nonce;
        }

        // Cancel result shape — a SEPARATE contract from Begin. Cancel is not
        // another Begin and has no ticket64: the native facade /
        // QaRequestCancel stamps result.token with the ORIGINAL input key on
        // every bridge result (including Rejected and cached non-Pending), so
        // a present (nonzero) token must be an exact valid 32-byte key AND
        // fieldwise equal to the input token frozen before the P/Invoke.
        // Non-Pending Cancel statuses do NOT require a zero token. The
        // all-zero token is honest only as the initialized None shape
        // (native preclaim stale/busy/argument) and proves neither accepted
        // cancellation nor GPU drain. Pure shape here: no invented token, no
        // rc/result mutation, no completion/cleanup classification.
        private static bool QaSharedCancelResultShapeValid(
            QaSharedBeginResultV1 r, QaSharedTokenV1 inputToken)
        {
            if (r.version != QaSharedWireVersionV1) return false;
            if (r.structBytes != QaSharedBeginResultBytesV1) return false;
            if (r.status > (uint)QaSharedExportStatusV1.QuarantinedRetained) return false;
            if (r.ledgerStateAfter > (uint)QaSharedLedgerStateV1.Quarantined) return false;
            if (r.getDataCallsThisCall > 1u) return false;
            if ((r.copyExecMask & ~3u) != 0u) return false;
            if ((r.copyOkMask & ~3u) != 0u) return false;
            if (QaSharedTokenIsPresent(r.token))
            {
                // exact valid 32-byte key, identical to the frozen input key
                if (!QaSharedPendingTokenShapeValid(r.token)) return false;
                if (!QaSharedTokenEquals(r.token, inputToken)) return false;
            }
            else
            {
                // zero token permitted ONLY for the initialized None shape
                if (r.status != (uint)QaSharedExportStatusV1.None) return false;
            }
            return true;
        }

        // ---- QA D9 internal wrappers (NOT called by any existing production
        // method). Local layout failure: rc -1, defaults out, shape=false, no
        // native touch. Otherwise exact capacities 192/80/64, 32/56/64,
        // 32/80. All actual native outputs are assigned FIRST, immediately
        // after the return, before any fallible check; the actual native rc is
        // preserved unchanged even for a malformed packet; missing
        // DLL/export exceptions propagate (never caught into a fallback).
        // No eager allocation, no BeginHosting, capture, Commit, Discard,
        // drain, finalize or Stop integration here.

        internal int QaBeginSharedGpu(
            ulong leftSurface, ulong rightSurface,
            ref SharedGpuCaptureIdentityV1 identity,
            out QaSharedBeginResultV1 result,
            out SharedGpuTicketV1 ticket,
            out bool readbackShapeValid)
        {
            result = default(QaSharedBeginResultV1);
            ticket = default(SharedGpuTicketV1);
            readbackShapeValid = false;
            if (!QaSharedWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            QaSharedBeginResultV1 actualResult;
            SharedGpuTicketV1 actualTicket;
            int rc = VRSIM_QaBeginSharedStereoV1(
                leftSurface, rightSurface,
                ref identity, SharedGpuIdentityBytesV1,
                out actualResult, QaSharedBeginResultBytesV1,
                out actualTicket, SharedGpuTicketBytesV1);
            // actual native outputs assigned FIRST, unconditionally preserved
            result = actualResult;
            ticket = actualTicket;
            // Actual nonzero ticket is shape-checked REGARDLESS of the native rc:
            // a contradictory/malformed nonzero ticket on a negative rc is still
            // preserved AND independently flagged here. A genuine Begin Pending
            // carries the token with a zero ticket. Shape only — never rc.
            bool ticketPresent = actualTicket.ticket != 0ul;
            readbackShapeValid = QaSharedBeginResultShapeValid(
                actualResult, actualTicket, ticketPresent);
            return rc;
        }

        internal int QaPollSharedGpu(
            ref QaSharedTokenV1 token,
            out QaSharedPollResultV1 result,
            out SharedGpuTicketV1 ticket,
            out bool readbackShapeValid)
        {
            result = default(QaSharedPollResultV1);
            ticket = default(SharedGpuTicketV1);
            readbackShapeValid = false;
            if (!QaSharedWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            QaSharedPollResultV1 actualResult;
            SharedGpuTicketV1 actualTicket;
            int rc = VRSIM_QaPollSharedStereoV1(
                ref token, QaSharedTokenBytesV1,
                out actualResult, QaSharedPollResultBytesV1,
                out actualTicket, SharedGpuTicketBytesV1);
            result = actualResult;
            ticket = actualTicket;
            // Actual nonzero ticket is shape-checked regardless of rc; the
            // validator still rejects ticketGranted==1 with a zero actual
            // ticket. Shape only — never completion/cleanup classification.
            bool ticketPresent = actualTicket.ticket != 0ul;
            readbackShapeValid = QaSharedPollResultShapeValid(
                actualResult, actualTicket, ticketPresent);
            return rc;
        }

        internal int QaCancelSharedGpu(
            ref QaSharedTokenV1 token,
            out QaSharedBeginResultV1 result,
            out bool readbackShapeValid)
        {
            result = default(QaSharedBeginResultV1);
            readbackShapeValid = false;
            if (!QaSharedWireLayoutValid())
                return (int)SharedGpuResultV1.BadArgument;
            // Input key frozen BEFORE the P/Invoke for the Cancel token
            // contract; the ref token itself is passed through untouched.
            QaSharedTokenV1 frozenToken = token;
            QaSharedBeginResultV1 actualResult;
            int rc = VRSIM_QaCancelSharedStereoV1(
                ref token, QaSharedTokenBytesV1,
                out actualResult, QaSharedBeginResultBytesV1);
            // actual native output preserved FIRST, before any validation
            result = actualResult;
            readbackShapeValid = QaSharedCancelResultShapeValid(actualResult, frozenToken);
            return rc;
        }
#endif // VRSIM_QA_D9_BEGIN_POLL
    }
}
