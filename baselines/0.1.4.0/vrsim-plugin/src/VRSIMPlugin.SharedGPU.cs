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
#if VRSIM_SHARED_2048
        private const uint SharedGpuTicketEdgeV1 = 2048u;
#else
        private const uint SharedGpuTicketEdgeV1 = 1536u;
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
    }
}
