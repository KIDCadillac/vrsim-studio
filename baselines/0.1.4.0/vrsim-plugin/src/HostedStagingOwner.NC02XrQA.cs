// NC02-02 source-only staging owner adapter: ALL new logic lives in this partial file.
// HostedCaptureOwner.NC02SourceQA.cs stays verbatim-except the four sanctioned minimal edits
// (namespace NC04 -> NC02SourceQA, sealed -> sealed partial, and the two pure-managed
// _stagingHelperDrained/_stagingPendingCount conditions added to StopVerified/TryCompleteStop).
// No producer, no XR, no NC01 pixel fallback: the NC02 helper is the ONLY readback path here.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;
using VisualComponents.Create3D;
using Matrix = VisualComponents.Create3D.Matrix;

namespace NC02XrQA
{
    // Frozen completed pair: one published result per accepted staging pair. Carries the immutable capture
    // ticket metadata plus the native 176B-collected evidence; NEVER any SDK/COM object or live pose read.
    // Static path (legacy): SourceLeaseEpoch=1 is a SYNTHETIC source-lease epoch (not XR) and
    // NativeXrEpoch/NativePoseSerial are pinned 0 because the NC02 source path never touches the headset pose
    // stream. XR-bound path: the result echoes ONLY the ticket's frozen NC02XrCaptureIdentityV1 numerics
    // (FrameSerial/NativeXrEpoch/NativePoseSerial/OriginalGeneration), SourceLeaseEpoch is 0 and no fresh
    // camera/pose/XR read happens at Collect time.
    public sealed class NC02SourcePairResultV1
    {
        public readonly ulong PairId;
        public readonly ulong FrameSerial;
        public readonly NC02XrCaptureIdentityV1 CaptureIdentity;
        public readonly ulong SourceLeaseEpoch;
        public readonly ulong NativeXrEpoch;
        public readonly ulong NativePoseSerial;
        public readonly int OwnerGeneration;
        public readonly int Width;
        public readonly int Height;
        public readonly uint SourceFormat;
        public readonly int QualityGeneration;
        public readonly byte[] LeftBgra;
        public readonly byte[] RightBgra;
        public readonly double[] LeftRawView16;
        public readonly double[] RightRawView16;
        public readonly double LeftFovY;
        public readonly double RightFovY;
        public readonly double Aspect;
        public readonly bool Perspective;
        public readonly string MainBefore;
        public readonly string MainAfter;
        public readonly ulong LeftSourceIdentity;
        public readonly ulong RightSourceIdentity;
        public readonly ulong DeviceIdentity;
        public readonly long CaptureStartQpc;
        public readonly long CaptureEndQpc;
        public readonly ulong CollectQpc;
        // Cumulative native-call durations (managed-measured): SubmitNativeMs includes the first Create when this
        // pair bootstrapped the session; CollectNativeMs includes every poll attempt made against this pair.
        public readonly double SubmitNativeMs;
        public readonly double CollectNativeMs;
        public readonly double EyeLockMs;
        public readonly double SurfaceRenderMs;
        public readonly double PairPrepareMs;
        public readonly double PairFinalizeMs;
        public readonly uint NativeSlotIndex;
        public readonly int NativeCollectedBytes;
        // Exact 176-byte image of THIS pair's actual native NC02CollectedPairV1 (all ten QPC/tick windows and
        // the three attempt counts included), serialized from the struct the Poll call returned. QA decodes
        // the real measurement from this array; NativeCollectedBytes alone is only the ABI size, never a trace.
        public readonly byte[] NativeCollectedRawBytes;
        public readonly string Scope;

        internal NC02SourcePairResultV1(NC01HostedSceneOwner.NC02StagingTicket ticket, NC01HostedSceneOwner.NC02CollectedPairV1 collected)
        {
            PairId = ticket.PairId;
            NC02XrCaptureIdentityV1 ticketIdentity = ticket.CaptureIdentity;
            if (ticketIdentity != null)
            {
                // XR-bound: republish ONLY the frozen ticket identity numerics into a fresh immutable copy —
                // no new camera/pose/main/XR-stats read, no ticket-QPC rewrite, no fresh frame/pose/epoch minting.
                CaptureIdentity = new NC02XrCaptureIdentityV1(ticketIdentity.FrameSerial, ticketIdentity.PoseSerial,
                    ticketIdentity.NativeXrEpoch, ticketIdentity.OriginalGeneration,
                    ticketIdentity.PoseSampleStartQpc, ticketIdentity.PoseSampleEndQpc, ticketIdentity.CaptureBeginQpc);
                FrameSerial = ticketIdentity.FrameSerial;
                SourceLeaseEpoch = 0UL; // this pair is NOT the synthetic static source-lease path
                NativeXrEpoch = ticketIdentity.NativeXrEpoch;
                NativePoseSerial = ticketIdentity.PoseSerial;
                OwnerGeneration = ticketIdentity.OriginalGeneration;
            }
            else
            {
                CaptureIdentity = null;
                FrameSerial = 0UL;
                SourceLeaseEpoch = 1UL;
                NativeXrEpoch = 0UL;
                NativePoseSerial = 0UL;
                OwnerGeneration = ticket.Generation;
            }
            Width = ticket.Width;
            Height = ticket.Height;
            SourceFormat = ticket.SourceFormat;
            QualityGeneration = ticket.QualityGeneration;
            LeftBgra = ticket.LeftBgra;
            RightBgra = ticket.RightBgra;
            LeftRawView16 = ticket.LeftRawView16;
            RightRawView16 = ticket.RightRawView16;
            LeftFovY = ticket.LeftFovY;
            RightFovY = ticket.RightFovY;
            Aspect = ticket.Aspect;
            Perspective = ticket.Perspective;
            MainBefore = ticket.MainBefore;
            MainAfter = ticket.MainAfter;
            LeftSourceIdentity = collected.leftSourceIdentity;
            RightSourceIdentity = collected.rightSourceIdentity;
            DeviceIdentity = collected.deviceIdentity;
            CaptureStartQpc = ticket.CaptureStartQpc;
            CaptureEndQpc = ticket.CaptureEndQpc;
            CollectQpc = collected.collectQpc;
            SubmitNativeMs = ticket.SubmitNativeMs;
            CollectNativeMs = ticket.CollectNativeMs;
            EyeLockMs = ticket.EyeLockMs;
            SurfaceRenderMs = ticket.SurfaceRenderMs;
            PairPrepareMs = ticket.PairPrepareMs;
            PairFinalizeMs = ticket.PairFinalizeMs;
            NativeSlotIndex = collected.slotIndex;
            NativeCollectedBytes = 176;
            NativeCollectedRawBytes = SerializeCollectedRaw(collected);
            Scope = ticketIdentity != null ? "QA_XR_BOUND_SOURCE_PAIR_NOT_DISPLAYED_ACCEPTANCE" : "QA_STATIC_REAL_SIM_SOURCE_NOT_XR";
        }

        // Pure managed blit of the already-returned 176B struct: no extra SDK/native query, no per-frame IO,
        // and the source struct itself is never mutated. Bound to this ticket's actual raw-view/FOV plus the
        // ticket-path identity above (static: pinned SourceLeaseEpoch=1 / NativeXrEpoch=0; XR: the frozen
        // capture identity echoed from ticket.CaptureIdentity).
        private static byte[] SerializeCollectedRaw(NC01HostedSceneOwner.NC02CollectedPairV1 collected)
        {
            int size = Marshal.SizeOf(typeof(NC01HostedSceneOwner.NC02CollectedPairV1));
            if (size != 176) throw new InvalidOperationException("NC02_STAGING_COLLECTED_RAW_SIZE:actual=" + size.ToString(CultureInfo.InvariantCulture));
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(collected, buffer, false);
                byte[] raw = new byte[size];
                Marshal.Copy(buffer, raw, 0, size);
                return raw;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
    }

    // Pure managed, IMMUTABLE XR capture identity, frozen by the CALLER (the real XR producer) at the actual
    // pose-sample/capture boundaries BEFORE submission. Numeric fields only: no setters, no object/SDK/COM
    // references, no VRSIMPlugin dependency and no PInvoke/callback in the constructor. The owner validates
    // these values plus the session lease; it cannot mint the identity itself and never claims to verify the
    // runtime origin of the caller's data — that guarantee stays with the producer that sampled it.
    public sealed class NC02XrCaptureIdentityV1
    {
        public readonly ulong FrameSerial, PoseSerial, NativeXrEpoch;
        public readonly int OriginalGeneration;
        public readonly long PoseSampleStartQpc, PoseSampleEndQpc, CaptureBeginQpc;

        public NC02XrCaptureIdentityV1(ulong frameSerial, ulong poseSerial, ulong nativeXrEpoch,
            int originalGeneration, long poseSampleStartQpc, long poseSampleEndQpc, long captureBeginQpc)
        {
            if (frameSerial == 0UL) throw new ArgumentOutOfRangeException("frameSerial");
            if (poseSerial == 0UL) throw new ArgumentOutOfRangeException("poseSerial");
            if (nativeXrEpoch == 0UL) throw new ArgumentOutOfRangeException("nativeXrEpoch");
            if (originalGeneration <= 0) throw new ArgumentOutOfRangeException("originalGeneration");
            if (poseSampleStartQpc <= 0L) throw new ArgumentOutOfRangeException("poseSampleStartQpc");
            if (poseSampleEndQpc < poseSampleStartQpc) throw new ArgumentOutOfRangeException("poseSampleEndQpc");
            if (captureBeginQpc < poseSampleEndQpc) throw new ArgumentOutOfRangeException("captureBeginQpc");
            FrameSerial = frameSerial;
            PoseSerial = poseSerial;
            NativeXrEpoch = nativeXrEpoch;
            OriginalGeneration = originalGeneration;
            PoseSampleStartQpc = poseSampleStartQpc;
            PoseSampleEndQpc = poseSampleEndQpc;
            CaptureBeginQpc = captureBeginQpc;
        }
    }

    public sealed partial class NC01HostedSceneOwner
    {
        // ------------------------------------------------------------------
        // NC02 native ABI (exact mirror of nc02-pair-staging-candidate/NC02PairStage.h: token 40B,
        // collected 176B, stop 56B, pack 8, cdecl). Export names below are copied from the header VERBATIM.
        // ------------------------------------------------------------------
#if VRSIM_NATIVE_QA
        internal const string Nc02HelperDllFileName = "NC02.PairStage.QA.dll";
        private const string Nc02HelperDllAbsolutePath = @"C:\Users\KID\Documents\ChatGPT\项目经理\reviews\vrsim-highres-h2\nc02-submit-kick-native-build-run1\NC02.PairStage.QA.dll";
#else
        internal const string Nc02HelperDllFileName = "NC02.PairStage.dll";
#endif
        private const ulong Nc02SourceLeaseEpoch = 1UL;
        private const int Nc02MaxPendingSlots = 2;
        // D3D9 pixel-format values used by the staging source validator. 22 is D3DFMT_X8R8G8B8
        // (NOT D3DFMT_R8G8B8, which is 23); 23 must never reach Create/SubmitPair.
        private const uint Nc02FormatA8R8G8B8 = 21u;   // D3DFMT_A8R8G8B8
        private const uint Nc02FormatX8R8G8B8 = 22u;   // D3DFMT_X8R8G8B8
        private const int Nc02SurfaceGetDescVtableSlot = 12; // official d3d9.h IDirect3DSurface9::GetDesc

        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct NC02PairTokenV1
        {
            public uint StructBytes, Version;
            public ulong PairSeq, PoseId, Epoch, OwnerGeneration;
        }

        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct NC02CollectedPairV1
        {
            public NC02PairTokenV1 token;
            public uint slotIndex, status;
            public ulong leftSourceIdentity, rightSourceIdentity, deviceIdentity;
            public uint width, height;
            public ulong submitQpc, collectQpc;
            public ulong getRenderTargetDataTicks, issueTicks, queryTicks, lockTicks, rowCopyTicks, unlockTicks, submitTotalTicks, pollTotalTicks;
            public uint queryAttempts, leftLockAttempts, rightLockAttempts, reserved;
        }

        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        private struct NC02StopStateV1
        {
            public uint structBytes, version, admissionOpen, callBusy, pendingSlots, liveOwnedResourceRefs, stopVerified, faulted;
            public int lastError;
            public uint reserved;
            public ulong createTicks, releaseTicks;
        }

        // Official D3DSURFACE_DESC order (d3d9.h): Format,Type,Usage,Pool,MultiSampleType,MultiSampleQuality,Width,Height; pack 4, sizeof 32.
        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        private struct NC02D3D9SurfaceDescV1
        {
            public uint Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height;
        }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int NC02SurfaceGetDescVtable(IntPtr surfaceSelf, out NC02D3D9SurfaceDescV1 desc);

        [DllImport(Nc02HelperDllFileName, EntryPoint = "NC02_PairStage_CreateV1", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NC02_PairStage_CreateV1(IntPtr borrowedSource, uint expectedW, uint expectedH,
            uint expectedFormat, ulong epoch, ulong generation, out ulong outHandle);

        [DllImport(Nc02HelperDllFileName, EntryPoint = "NC02_PairStage_SubmitPairV1", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NC02_PairStage_SubmitPairV1(ulong handle, IntPtr borrowedLeft, IntPtr borrowedRight,
            ref NC02PairTokenV1 token, uint tokenBytes);

        [DllImport(Nc02HelperDllFileName, EntryPoint = "NC02_PairStage_PollCollectV1", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NC02_PairStage_PollCollectV1(ulong handle,
            [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] byte[] outL, ulong capL,
            [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] byte[] outR, ulong capR,
            ref NC02CollectedPairV1 result, uint resultCapacity);

        [DllImport(Nc02HelperDllFileName, EntryPoint = "NC02_PairStage_StopStepV1", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NC02_PairStage_StopStepV1(ulong handle, ref NC02StopStateV1 result, uint resultCapacity);

        [DllImport(Nc02HelperDllFileName, EntryPoint = "NC02_PairStage_DestroyV1", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NC02_PairStage_DestroyV1(ulong handle);

        // ------------------------------------------------------------------
        // Staging state. All decision flips stay on the SAME short _gateLock used by the legacy admission
        // surface; native/SDK/Unlock work NEVER runs inside it. Defaults (_stagingHelperDrained=true,
        // _stagingPendingCount=0) keep every legacy stop path byte-for-byte honest when the new APIs are unused.
        // ------------------------------------------------------------------
        internal sealed class NC02StagingTicket
        {
            public ulong PairId;
            public int Generation;
            public double[] LeftRawView16, RightRawView16;
            public double LeftFovY, RightFovY, Aspect;
            public bool Perspective;
            public int Width, Height;
            public uint SourceFormat;
            public int QualityGeneration;
            public string MainBefore;
            public ulong LeftSourceIdentity, RightSourceIdentity;
            public long CaptureStartQpc, CaptureEndQpc;
            public double SubmitNativeMs, EyeLockMs, SurfaceRenderMs, PairPrepareMs;
            public byte[] LeftBgra, RightBgra;
            // Second (and only) fill point: finalize evidence freezes the ticket before any publish is possible.
            public string MainAfter;
            public double PairFinalizeMs;
            public double CollectNativeMs;
            public bool FinalFrozen;
            // XR capture identity: a PRIVATE numeric copy of the caller-frozen identity (null for the legacy
            // static path). SubmittedPoseId/SubmittedEpoch record the ACTUAL token values handed to native
            // (XR: identity pose/epoch; static: 0/1) so the generic Collect check compares against what was
            // really submitted, never against a guessed constant.
            public NC02XrCaptureIdentityV1 CaptureIdentity;
            public ulong SubmittedPoseId, SubmittedEpoch;

            public void FreezeFinalMetadata(string afterSerialized, double pairFinalizeMs)
            {
                if (FinalFrozen) throw new InvalidOperationException("NC02_STAGING_TICKET_DOUBLE_FREEZE:pairId=" + PairId.ToString(CultureInfo.InvariantCulture));
                MainAfter = afterSerialized;
                PairFinalizeMs = pairFinalizeMs;
                FinalFrozen = true;
            }
        }

        private readonly NC02StagingTicket[] _stagingTickets = new NC02StagingTicket[Nc02MaxPendingSlots];
        private int _stagingTicketHead, _stagingTicketTail;
        private int _stagingCallBusy;
        private int _stagingPendingCount;
        private bool _stagingHelperDrained = true;
        private ulong _stagingHelperHandle;
        private ulong _stagingNextPairSeq = 1UL;
        // XR session binding (pure managed): the ACTUAL epoch/mode of the live helper session, frozen at the
        // first successful Create, and the highest ACCEPTED XR FrameSerial. PairId above stays the LOCAL helper
        // sequence and never substitutes for the global XR FrameSerial. All three clear only after Destroy rc==0.
        private ulong _stagingSessionEpoch;                    // 0 = no Create yet; else the actual Create epoch
        private bool _stagingSessionUsesXrIdentity;            // the mode of the actual successful Create
        private ulong _stagingLastAcceptedXrFrameSerial;
        // Diagnostics only: cumulative managed-measured first-Create duration. Never a correctness input.
        private double _stagingCreateMsTotal = 0.0;

        private static readonly object Nc02ModuleLock = new object();
        private static bool _nc02ModuleChecked;

        // Pure managed diagnostics.
        public int StagingPendingCount { get { return Volatile.Read(ref _stagingPendingCount); } }
        public bool StagingHelperDrained { get { lock (_gateLock) { return _stagingHelperDrained; } } }
        public ulong StagingHelperHandle { get { lock (_gateLock) { return _stagingHelperHandle; } } }

        // ------------------------------------------------------------------
        // TrySubmitStagingRawPair
        // Gates mirror the legacy CaptureRawPair(generation) entry; busy registration uses the SAME
        // _runtimeCallsBusy/_captureBusy fields (plus a dedicated staging flag) so the legacy StopVerified
        // cannot report completion mid-transaction. FULL is a plain false with NO SDK call, NO token consumed
        // and NO seq write. Parameter errors are plain throws; Stop/cancel never writes a sticky fault.
        // ------------------------------------------------------------------
        public bool TrySubmitStagingRawPair(Matrix leftRawView, Matrix rightRawView, double leftFov, double rightFov,
            double aspect, bool perspective, int generation, Func<bool> shouldStop, out ulong pairId, out string reason)
        {
            // Legacy static entry: delegates with identity==null — the ONLY caller allowed the synthetic
            // epoch-1/pose-0 token. A session already XR-bound refuses it via the mode check below, so a
            // static submission can never pollute an XR-bound session.
            return TrySubmitStagingRawPairInternal(leftRawView, rightRawView, leftFov, rightFov, aspect, perspective,
                generation, shouldStop, null, out pairId, out reason);
        }

        // XR entry: the SAME two-lock native transaction under a caller-frozen real XR capture identity.
        // identity==null is a hard ArgumentNullException here — XR input validation failure NEVER falls back
        // to the static path. The owner validates numeric fields and the session lease only; it does not
        // fabricate identities and cannot attest the runtime origin of the caller's data (that promise
        // belongs to the producer that froze the identity at the real read/capture boundaries).
        public bool TrySubmitXrStagingRawPair(Matrix leftRawView, Matrix rightRawView, double leftFov, double rightFov,
            double aspect, bool perspective, int generation, Func<bool> shouldStop, NC02XrCaptureIdentityV1 identity,
            out ulong pairId, out string reason)
        {
            if (identity == null) throw new ArgumentNullException("identity");
            RequireXrCaptureIdentityShape(identity, generation);
            return TrySubmitStagingRawPairInternal(leftRawView, rightRawView, leftFov, rightFov, aspect, perspective,
                generation, shouldStop, identity, out pairId, out reason);
        }

        // Plain numeric validation BEFORE any Matrix indexer/SDK/Create/Submit: a bad identity or an outsider
        // generation throws with zero state change (no busy registration, no ticket, no pairSeq consumed).
        private static void RequireXrCaptureIdentityShape(NC02XrCaptureIdentityV1 identity, int generation)
        {
            if (identity.FrameSerial == 0UL || identity.PoseSerial == 0UL || identity.NativeXrEpoch == 0UL)
                throw new InvalidOperationException("NC02_STAGING_XR_IDENTITY_NONZERO_VIOLATION:frameSerial=" + FormatInvariant(identity.FrameSerial)
                    + "|poseSerial=" + FormatInvariant(identity.PoseSerial) + "|nativeXrEpoch=" + FormatInvariant(identity.NativeXrEpoch));
            if (identity.OriginalGeneration <= 0 || identity.PoseSampleStartQpc <= 0L
                || identity.PoseSampleEndQpc < identity.PoseSampleStartQpc || identity.CaptureBeginQpc < identity.PoseSampleEndQpc)
                throw new InvalidOperationException("NC02_STAGING_XR_IDENTITY_ORDER_VIOLATION:generation=" + FormatInvariant(identity.OriginalGeneration)
                    + "|poseStartQpc=" + FormatInvariant(identity.PoseSampleStartQpc) + "|poseEndQpc=" + FormatInvariant(identity.PoseSampleEndQpc)
                    + "|captureBeginQpc=" + FormatInvariant(identity.CaptureBeginQpc));
            if (identity.OriginalGeneration != generation)
                throw new InvalidOperationException("NC02_STAGING_XR_IDENTITY_GENERATION_MISMATCH:identityGeneration=" + FormatInvariant(identity.OriginalGeneration)
                    + "|requestedGeneration=" + FormatInvariant(generation));
        }

        private bool TrySubmitStagingRawPairInternal(Matrix leftRawView, Matrix rightRawView, double leftFov, double rightFov,
            double aspect, bool perspective, int generation, Func<bool> shouldStop, NC02XrCaptureIdentityV1 identity,
            out ulong pairId, out string reason)
        {
            pairId = 0UL; reason = null;
            if (shouldStop == null) throw new ArgumentNullException("shouldStop");
            if (!_runtimeBackend) throw new InvalidOperationException("NC02_STAGING_RUNTIME_BACKEND_REQUIRED:submit");
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            if (!perspective) throw new InvalidOperationException("NC02_STAGING_PERSPECTIVE_REQUIRED:perspective=0");
            RequireRawFovY(0, leftFov);
            RequireRawFovY(1, rightFov);
            RequireRawSourceAspect(aspect);
            VerifyAccess();
            int frozenQualityGeneration = 0;
            bool xrSessionModeChanged = false;
            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) throw new InvalidOperationException("NC02_STAGING_SUBMIT_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + FormatInvariant(generation));
                if (Volatile.Read(ref _stopRequested) != 0) { reason = "NC02_STAGING_SUBMIT_STOP_REQUESTED:generation=" + FormatInvariant(generation); return false; }
                if (Volatile.Read(ref _admissionOpen) == 0) throw new InvalidOperationException("NC02_STAGING_SUBMIT_ADMISSION_CLOSED:generation=" + FormatInvariant(generation));
                if (Volatile.Read(ref _hostingState) != 2) throw new InvalidOperationException("NC02_STAGING_SUBMIT_NOT_HOSTED:hostingState=" + FormatInvariant(Volatile.Read(ref _hostingState)));
                if (_captureDisabled) throw new InvalidOperationException("NC02_STAGING_SUBMIT_CAPTURE_DISABLED:" + _disabledReason);
                if (_qualityGeneration != 1) throw new InvalidOperationException("NC02_STAGING_SUBMIT_QUALITY_NOT_FROZEN:generation=" + FormatInvariant(_qualityGeneration));
                // Slot-full refusal happens on the SAME locked snapshot BEFORE any SDK/native call and before the
                // busy registrations, so a FULL rejection cannot overwrite a live token or consume a pair seq.
                if (Volatile.Read(ref _stagingPendingCount) >= Nc02MaxPendingSlots) { reason = "NC02_STAGING_SUBMIT_FULL:pending=" + FormatInvariant(Volatile.Read(ref _stagingPendingCount)) + "|slots=2|sdkCalls=0"; return false; }
                if (Volatile.Read(ref _stagingCallBusy) != 0) { reason = "NC02_STAGING_SUBMIT_CALL_BUSY:generation=" + FormatInvariant(generation); return false; }
                if (Volatile.Read(ref _runtimeCallsBusy) != 0) { reason = "NC02_STAGING_SUBMIT_RUNTIME_CALL_BUSY:generation=" + FormatInvariant(generation); return false; }
                if (Volatile.Read(ref _captureBusy) != 0) { reason = "NC02_STAGING_SUBMIT_CAPTURE_BUSY:generation=" + FormatInvariant(generation); return false; }
                // XR session-binding / FrameSerial refusals on the SAME locked snapshot, BEFORE the busy
                // registration: plain refusal, no SDK, no new/overwrite ticket, no pairSeq consumed and never a
                // re-bind of the live handle. A static (identity==null) submit keeps the epoch-1/xr=false lease.
                bool sessionLive = _stagingHelperHandle != 0UL || !_stagingHelperDrained || _stagingSessionEpoch != 0UL;
                ulong wantSessionEpoch = identity != null ? identity.NativeXrEpoch : Nc02SourceLeaseEpoch;
                bool wantSessionXr = identity != null;
                if (sessionLive && (_stagingSessionEpoch != wantSessionEpoch || _stagingSessionUsesXrIdentity != wantSessionXr))
                {
                    xrSessionModeChanged = true;
                }
                else if (identity != null && identity.FrameSerial <= _stagingLastAcceptedXrFrameSerial)
                {
                    // Ordinary parameter/duplicate-frame refusal: plain false, no SDK, no frame minting.
                    reason = "NC02_STAGING_SUBMIT_XR_FRAME_SERIAL_NOT_NEW:requested=" + FormatInvariant(identity.FrameSerial)
                        + "|lastAccepted=" + FormatInvariant(_stagingLastAcceptedXrFrameSerial) + "|sdkCalls=0|tickets=0|pairSeqConsumed=0";
                    return false;
                }
                else
                {
                    _runtimeCallsBusy = 1;
                    _captureBusy = 1;
                    Volatile.Write(ref _stagingCallBusy, 1);
                    frozenQualityGeneration = _qualityGeneration;
                }
            }
            if (xrSessionModeChanged)
            {
                // Refuse with a PURE managed RequestStop at the original generation: an XR epoch change or
                // static/XR mixing means the live session must be drained first. Never Create a new native
                // session for the mismatch and never re-send an old ticket under a fresher epoch.
                RequestStop(generation);
                reason = "NC02_STAGING_SUBMIT_SESSION_EPOCH_OR_MODE_CHANGED:frozenEpoch=" + FormatInvariant(_stagingSessionEpoch)
                    + "|frozenXr=" + Bool(_stagingSessionUsesXrIdentity)
                    + "|requestedEpoch=" + FormatInvariant(identity != null ? identity.NativeXrEpoch : Nc02SourceLeaseEpoch)
                    + "|requestedXr=" + Bool(identity != null) + "|drainRequired=1|sdkCalls=0|tickets=0|pairSeqConsumed=0";
                return false;
            }
            try
            {
                // Like the legacy CaptureRawPair entry, the SDK Matrix indexer reads + finiteness validation run
                // OUTSIDE the sticky capture-fault catch: whole busy is registered and the dedicated INPUT_MATRIX
                // group is granted first, but non-finite caller input is a plain throw — no Disable, no
                // Create/Submit — and the outer finally still restores every busy flag.
                RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_INPUT_MATRIX", true, "NC02_STAGING_STOP_REQUESTED");
                double[] left16 = RequireRawView16Finite(0, leftRawView);
                double[] right16 = RequireRawView16Finite(1, rightRawView);
                try { return TrySubmitStagingRawPairCore(generation, leftRawView, rightRawView, leftFov, rightFov, aspect, perspective, shouldStop, identity, frozenQualityGeneration, left16, right16, out pairId, out reason); }
                catch (OperationCanceledException)
                {
                    // Cancel never writes a sticky fault, but a post-Create/accept cancel must still admit the
                    // pure-managed drain entry for whatever ticket is already registered.
                    RequestStop(generation);
                    throw;
                }
                catch
                {
                    // Every escaping fault (post-accept snapshot/finalize throws included) admits the drain entry
                    // BEFORE the sticky Disable bookkeeping; the accepted ticket then stays pending/drainable.
                    RequestStop(generation);
                    if (!_captureDisabled) DisableCaptures("NC02_STAGING_SUBMIT_FAULT");
                    throw;
                }
            }
            finally { lock (_gateLock) { Volatile.Write(ref _captureBusy, 0); Volatile.Write(ref _runtimeCallsBusy, 0); Volatile.Write(ref _stagingCallBusy, 0); } }
        }

        // identity==null selects the legacy static token lease (poseId=0/epoch=1); a non-null identity is the
        // caller-frozen XR capture identity already shape-validated by the XR public entry. The static public
        // entry is the only path that may pass null.
        private bool TrySubmitStagingRawPairCore(int generation, Matrix leftRawView, Matrix rightRawView,
            double leftFov, double rightFov, double aspect, bool perspective, Func<bool> shouldStop,
            NC02XrCaptureIdentityV1 identity, int frozenQualityGeneration, double[] left16, double[] right16, out ulong pairId, out string reason)
        {
            pairId = 0UL; reason = null;
            Stopwatch prepareSw = Stopwatch.StartNew();
            // left16/right16 were read+verified by the outer entry inside the INPUT_MATRIX group; Core never
            // touches the SDK Matrix indexer again.
            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_PREPARE", true, "NC02_STAGING_STOP_REQUESTED");
            VerifyRuntimeIdentityBindings();
            NC01MainSnapshotV1 before = MainSnapshotCore(); // MainSnapshot #1 of exactly 2; main camera never written.

            string readyReason;
            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_READY0", true, "NC02_STAGING_STOP_REQUESTED");
            if (!TryEyeReady(0, out readyReason) || !TryConfirmEyeStageBaseline(0, out readyReason))
                throw new InvalidOperationException("NC02_STAGING_PREPARE_REFUSED:eye=0|reason=" + readyReason);
            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_READY1", true, "NC02_STAGING_STOP_REQUESTED");
            if (!TryEyeReady(1, out readyReason) || !TryConfirmEyeStageBaseline(1, out readyReason))
                throw new InvalidOperationException("NC02_STAGING_PREPARE_REFUSED:eye=1|reason=" + readyReason);

            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_QUALITY_BUILD", true, "NC02_STAGING_STOP_REQUESTED");
            string pairFingerprint, pairBuildError;
            string[] pairSubset;
            if (!TryBuildRuntimeQualityFingerprint(before.Serialized, out pairFingerprint, out pairSubset, out pairBuildError))
            { DisableCaptures(pairBuildError); throw new InvalidOperationException(_disabledReason); }
            if (!string.Equals(_runtimeQualityFingerprint, pairFingerprint, StringComparison.Ordinal))
            { DisableCaptures("NC02_STAGING_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(pairSubset)); throw new InvalidOperationException(_disabledReason); }

            // Raw camera set + Update + actual16/FOV/aspect/ortho/dims validation, each eye in its own admitted group.
            double[] actualLeft16; double actualLeftFov; double actualLeftAspect; bool actualLeftOrtho;
            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_CAMERA_SET_LEFT", true, "NC02_STAGING_STOP_REQUESTED");
            ICamera leftCam = _eyes[0].Camera;
            if (leftCam == null) throw new InvalidOperationException("NC02_STAGING_EYE_CAMERA_NULL:eye=0");
            leftCam.IsOrthographic = false;
            leftCam.AspectRatio = aspect;
            leftCam.FieldOfViewY = leftFov;
            leftCam.ViewMatrix = leftRawView;
            leftCam.Update();
            ValidateRawEyeProjection(0, leftCam, left16, leftFov, aspect, "staging-post-update", out actualLeft16, out actualLeftFov, out actualLeftAspect, out actualLeftOrtho);

            double[] actualRight16; double actualRightFov; double actualRightAspect; bool actualRightOrtho;
            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_CAMERA_SET_RIGHT", true, "NC02_STAGING_STOP_REQUESTED");
            ICamera rightCam = _eyes[1].Camera;
            if (rightCam == null) throw new InvalidOperationException("NC02_STAGING_EYE_CAMERA_NULL:eye=1");
            rightCam.IsOrthographic = false;
            rightCam.AspectRatio = aspect;
            rightCam.FieldOfViewY = rightFov;
            rightCam.ViewMatrix = rightRawView;
            rightCam.Update();
            ValidateRawEyeProjection(1, rightCam, right16, rightFov, aspect, "staging-post-update", out actualRight16, out actualRightFov, out actualRightAspect, out actualRightOrtho);
            // The entire two-lock transaction below (both TryLocks, both Renders, GetDesc, Create, the single
            // SubmitPair and the finally-unlocks) runs as ONE admitted group. If Stop lands during
            // rightCam.Update, this grant is refused and NO native work starts for this pair.
            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_RENDER_SUBMIT", true, "NC02_STAGING_STOP_REQUESTED");
            double prepareMs = prepareSw.Elapsed.TotalMilliseconds;
            prepareSw.Stop();

            // ---------------- two-lock transaction ----------------
            // Left TryLock(10ms) then right TryLock(10ms); a failed second lock immediately releases the first
            // with NO render/copy/Submit. While BOTH locks are held: one SurfaceRender per eye, fresh actual
            // _scene reads, post-render projection + stage re-verification, GetDesc source validation, first
            // Create (session bootstrap) and exactly one SubmitPair. Unlock ALWAYS runs in the finally, each eye
            // in its own try so a right-side Unlock throw can never leak the left lock.
            bool leftLocked = false, rightLocked = false, accepted = false, transactionThrew = false;
            NC02StagingTicket ticket = null;
            ulong acceptedPairId = 0UL;
            double eyeLockMs = 0.0, surfaceRenderMs = 0.0, submitNativeMs = 0.0;
            long captureStartQpc = 0L, captureEndQpc = 0L;
            string unlockFaultEvidence = null;
            IntPtr sceneLeft = IntPtr.Zero, sceneRight = IntPtr.Zero;
            try
            {
                Stopwatch sw = Stopwatch.StartNew();
                leftLocked = _images[0].TryLock(new Duration(TimeSpan.FromMilliseconds(10)));
                sw.Stop(); eyeLockMs += sw.Elapsed.TotalMilliseconds;
                if (!leftLocked) { reason = "NC02_STAGING_EYE0_LOCK_TIMEOUT:10ms|sdkCalls=0"; return false; }

                sw = Stopwatch.StartNew();
                rightLocked = _images[1].TryLock(new Duration(TimeSpan.FromMilliseconds(10)));
                sw.Stop(); eyeLockMs += sw.Elapsed.TotalMilliseconds;
                if (!rightLocked) { reason = "NC02_STAGING_EYE1_LOCK_TIMEOUT:10ms|leftUnlocked=1|renders=0|submit=0"; return false; }

                int renderRcLeft;
                sw.Restart();
                try { renderRcLeft = _app.SurfaceRender(_eyes[0]); } finally { sw.Stop(); surfaceRenderMs += sw.Elapsed.TotalMilliseconds; }
                if (renderRcLeft < 0) throw new InvalidOperationException("NC02_STAGING_SURFACE_RENDER_FAILED:eye=0|actualrc=" + FormatInvariant(renderRcLeft));
                sceneLeft = RequireScenePointer(0);

                int renderRcRight;
                sw.Restart();
                try { renderRcRight = _app.SurfaceRender(_eyes[1]); } finally { sw.Stop(); surfaceRenderMs += sw.Elapsed.TotalMilliseconds; }
                if (renderRcRight < 0) throw new InvalidOperationException("NC02_STAGING_SURFACE_RENDER_FAILED:eye=1|actualrc=" + FormatInvariant(renderRcRight));
                sceneRight = RequireScenePointer(1);
                // Fresh actual re-read of BOTH _scene pointers after the right-eye render; the left pointer is
                // re-verified, never blindly reused.
                if (RequireScenePointer(0) != sceneLeft) throw new InvalidOperationException("NC02_STAGING_SCENE_IDENTITY_DRIFT:eye=0");
                if (sceneLeft == sceneRight) throw new InvalidOperationException("NC02_STAGING_SOURCES_NOT_DISTINCT:leftHex=" + HexPointer(sceneLeft));

                ValidateRawEyeProjection(0, _eyes[0].Camera, left16, leftFov, aspect, "staging-lock-post-render", out actualLeft16, out actualLeftFov, out actualLeftAspect, out actualLeftOrtho);
                ValidateRawEyeProjection(1, _eyes[1].Camera, right16, rightFov, aspect, "staging-lock-post-render", out actualRight16, out actualRightFov, out actualRightAspect, out actualRightOrtho);
                EnsureEyeStagesMatchBaseline(0);
                EnsureEyeStagesMatchBaseline(1);

                NC02D3D9SurfaceDescV1 descLeft = RequireValidatedStagingSurfaceDesc(0, sceneLeft, TargetWidth, TargetHeight);
                NC02D3D9SurfaceDescV1 descRight = RequireValidatedStagingSurfaceDesc(1, sceneRight, TargetWidth, TargetHeight);
                if (descLeft.Format != descRight.Format)
                    throw new InvalidOperationException("NC02_STAGING_FORMAT_SPLIT:left=" + FormatInvariant(descLeft.Format) + "|right=" + FormatInvariant(descRight.Format));

                ulong newPairId = _stagingNextPairSeq;

                // Pre-allocate EVERY managed object the accepted pair needs, BEFORE entering native Create:
                // the ticket, both private BGRA targets and all metadata. An OOM here is a clean pre-native
                // throw, never an accepted native pair with pending=0 and no discard/drain-able ticket.
                ticket = new NC02StagingTicket();
                ticket.PairId = newPairId;
                ticket.Generation = generation;
                ticket.LeftRawView16 = actualLeft16;    // caller arrays are never reused afterwards and
                ticket.RightRawView16 = actualRight16;  // no gamma/alpha rewrite happens on them
                ticket.LeftFovY = actualLeftFov;
                ticket.RightFovY = actualRightFov;
                ticket.Aspect = actualLeftAspect;
                ticket.Perspective = true;
                ticket.Width = (int)descLeft.Width;
                ticket.Height = (int)descLeft.Height;
                ticket.SourceFormat = descLeft.Format;
                ticket.QualityGeneration = frozenQualityGeneration;
                ticket.MainBefore = before.Serialized;
                ticket.LeftSourceIdentity = (ulong)sceneLeft.ToInt64();
                ticket.RightSourceIdentity = (ulong)sceneRight.ToInt64();
                ticket.EyeLockMs = eyeLockMs;
                ticket.SurfaceRenderMs = surfaceRenderMs;
                ticket.PairPrepareMs = prepareMs;
                ticket.LeftBgra = new byte[checked(TargetWidth * TargetHeight * 4)];
                ticket.RightBgra = new byte[checked(TargetWidth * TargetHeight * 4)];
                // XR identity: the PRIVATE numeric copy is frozen into the ticket BEFORE any native Create/Submit,
                // together with the ACTUAL submitted token values (static: pose 0 / lease epoch 1).
                ticket.CaptureIdentity = (identity == null) ? null : new NC02XrCaptureIdentityV1(
                    identity.FrameSerial, identity.PoseSerial, identity.NativeXrEpoch, identity.OriginalGeneration,
                    identity.PoseSampleStartQpc, identity.PoseSampleEndQpc, identity.CaptureBeginQpc);
                ticket.SubmittedPoseId = (identity == null) ? 0UL : identity.PoseSerial;
                ticket.SubmittedEpoch = (identity == null) ? Nc02SourceLeaseEpoch : identity.NativeXrEpoch;

                NC02PairTokenV1 token = default(NC02PairTokenV1);
                token.StructBytes = 40u;
                token.Version = 1u;
                token.PairSeq = newPairId;  // LOCAL helper sequence; the global XR FrameSerial lives in the identity
                token.PoseId = ticket.SubmittedPoseId;
                token.Epoch = ticket.SubmittedEpoch;
                token.OwnerGeneration = (ulong)generation;

                ulong handle;
                lock (_gateLock) { handle = _stagingHelperHandle; }
                if (handle == 0UL)
                {
                    // Load-and-verify happens exactly once, BEFORE the first Create; a failed load is a clean
                    // managed rejection (no session, no drained flip, nothing to orphan).
                    EnsureNc02HelperLoaded();
                    Stopwatch createSw = Stopwatch.StartNew();
                    int createRc;
                    try { createRc = NC02_PairStage_CreateV1(sceneLeft, (uint)TargetWidth, (uint)TargetHeight, descLeft.Format, ticket.SubmittedEpoch, (ulong)generation, out handle); }
                    finally { createSw.Stop(); }
                    if (createRc != 0) throw new InvalidOperationException("NC02_STAGING_CREATE_FAILED:rc=" + FormatInvariant(createRc) + "|format=" + FormatInvariant(descLeft.Format));
                    lock (_gateLock)
                    {
                        // Create succeeded: the session is live from this instant and drained=false stays honest.
                        // The cached session epoch/mode is the ACTUAL value handed to this Create (ticket was
                        // frozen above); a failed Create never caches a phantom new epoch/mode.
                        _stagingHelperHandle = handle;
                        _stagingHelperDrained = false;
                        _stagingSessionEpoch = ticket.SubmittedEpoch;
                        _stagingSessionUsesXrIdentity = ticket.CaptureIdentity != null;
                        _stagingCreateMsTotal += createSw.Elapsed.TotalMilliseconds;
                    }
                    submitNativeMs += createSw.Elapsed.TotalMilliseconds;
                }

                // CaptureStartQpc/EndQpc bracket ONLY the native SubmitPair call (already AFTER camera setup,
                // SurfaceRender and all allocations) — the REAL pose-sample/capture-begin times come from the
                // caller-frozen QPCs inside ticket.CaptureIdentity, not from this narrow submit window.
                captureStartQpc = Stopwatch.GetTimestamp();
                Stopwatch submitSw = Stopwatch.StartNew();
                int submitRc;
                try { submitRc = NC02_PairStage_SubmitPairV1(handle, sceneLeft, sceneRight, ref token, 40u); }
                finally { submitSw.Stop(); captureEndQpc = Stopwatch.GetTimestamp(); }
                submitNativeMs += submitSw.Elapsed.TotalMilliseconds;
                if (submitRc != 0)
                {
                    // The token was presented, so the seq advances to keep pairSeq strictly increasing.
                    lock (_gateLock) { _stagingNextPairSeq = newPairId + 1UL; }
                    if (submitRc == 1) throw new InvalidOperationException("NC02_STAGING_SUBMIT_NATIVE_FULL_UNEXPECTED:pairSeq=" + FormatInvariant(newPairId));
                    throw new InvalidOperationException("NC02_STAGING_SUBMIT_FAILED:rc=" + FormatInvariant(submitRc) + "|pairSeq=" + FormatInvariant(newPairId) + "|drainRequired=1");
                }

                // rc==0: register the fully prepared ticket IMMEDIATELY via only non-allocating writes plus ONE
                // short lock (2-ring store, pending++, seq++). No SDK read, no shouldStop and no string work
                // may precede the registration, so even a Stop landing right now leaves a drain-able ticket.
                ticket.CaptureStartQpc = captureStartQpc;
                ticket.CaptureEndQpc = captureEndQpc;
                ticket.SubmitNativeMs = submitNativeMs;
                acceptedPairId = newPairId;
                lock (_gateLock)
                {
                    _stagingTickets[_stagingTicketTail] = ticket;
                    _stagingTicketTail = (_stagingTicketTail + 1) % Nc02MaxPendingSlots;
                    Volatile.Write(ref _stagingPendingCount, Volatile.Read(ref _stagingPendingCount) + 1);
                    _stagingNextPairSeq = newPairId + 1UL;
                    // Same short lock, still purely numeric: an ACCEPTED XR pair advances the monotonic
                    // FrameSerial water-mark. The static path (null identity) never touches it.
                    if (ticket.CaptureIdentity != null)
                        _stagingLastAcceptedXrFrameSerial = ticket.CaptureIdentity.FrameSerial;
                }
                accepted = true;
            }
            catch { transactionThrew = true; throw; }
            finally
            {
                Exception rightUnlockFault = null, leftUnlockFault = null;
                if (rightLocked) { try { _images[1].Unlock(); } catch (Exception ex) { rightUnlockFault = ex; } }
                if (leftLocked) { try { _images[0].Unlock(); } catch (Exception ex) { leftUnlockFault = ex; } }
                if (rightUnlockFault != null || leftUnlockFault != null)
                {
                    unlockFaultEvidence = "rightUnlock=" + DescribeStagingFault(rightUnlockFault) + "|leftUnlock=" + DescribeStagingFault(leftUnlockFault);
                    if (!accepted && !transactionThrew)
                        throw new InvalidOperationException("NC02_STAGING_UNLOCK_FAULT_PRE_ACCEPT:" + unlockFaultEvidence);
                    // An original transaction throw keeps its own diagnosis; a post-accept unlock fault is
                    // handled below so the accepted slot still gets a drain-able ticket instead of vanishing.
                }
            }

            if (accepted && unlockFaultEvidence != null)
            {
                DisableCaptures("NC02_STAGING_UNLOCK_FAULT_POST_ACCEPT:" + unlockFaultEvidence);
                RequestStop(generation);
                reason = _disabledReason + "|pairId=" + FormatInvariant(acceptedPairId) + "|drainRequired=1";
                return false;
            }

            // Post-acceptance phase: any non-stop throw must still close the lease so the registered ticket
            // can be drained; the ticket is never orphaned and nothing half-published is returned.
            try
            {
                RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_FINAL_SNAPSHOT", true, "NC02_STAGING_STOP_REQUESTED");
            }
            catch (OperationCanceledException)
            {
                // Post-accept cancel: no sticky fault, but the registered ticket MUST get a drain-entry admission.
                RequestStop(generation);
                reason = "NC02_STAGING_SUBMIT_STOPPED_POST_ACCEPT:pairId=" + FormatInvariant(acceptedPairId) + "|drainRequired=1|published=0";
                return false;
            }
            NC01MainSnapshotV1 after = MainSnapshotCore(); // MainSnapshot #2 of exactly 2.
            Stopwatch finalizeSw = Stopwatch.StartNew();
            if (!after.StrictEquals(before))
            {
                DisableCaptures("NC02_STAGING_SNAPSHOT_MISMATCH");
                RequestStop(generation);
                reason = _disabledReason + "|pairId=" + FormatInvariant(acceptedPairId) + "|drainRequired=1";
                return false;
            }
            RequireRuntimeGroup(generation, shouldStop, "NC02_STAGING_GROUP_POST_GATE", true, "NC02_STAGING_STOP_REQUESTED");
            EnsureEyeStagesMatchBaseline(0);
            EnsureEyeStagesMatchBaseline(1);
            string postFingerprint, postBuildError;
            string[] postSubset;
            if (!TryBuildRuntimeQualityFingerprint(after.Serialized, out postFingerprint, out postSubset, out postBuildError))
            { DisableCaptures(postBuildError); RequestStop(generation); reason = _disabledReason + "|pairId=" + FormatInvariant(acceptedPairId) + "|drainRequired=1"; return false; }
            if (!string.Equals(_runtimeQualityFingerprint, postFingerprint, StringComparison.Ordinal))
            { DisableCaptures("NC02_STAGING_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(postSubset)); RequestStop(generation); reason = _disabledReason + "|pairId=" + FormatInvariant(acceptedPairId) + "|drainRequired=1"; return false; }
            VerifyRuntimeIdentityBindings();
            finalizeSw.Stop();
            ticket.FreezeFinalMetadata(after.Serialized, finalizeSw.Elapsed.TotalMilliseconds);
            if (shouldStop())
            {
                RequestStop(generation);
                reason = "NC02_STAGING_SUBMIT_STOPPED_POST_ACCEPT:pairId=" + FormatInvariant(acceptedPairId) + "|drainRequired=1";
                return false;
            }
            bool commitStopCancel;
            string commitRejectReason;
            bool committed;
            lock (_gateLock)
            {
                committed = RuntimeGroupGrant(generation, "NC02_STAGING_SUBMIT_COMMIT", true, out commitStopCancel, out commitRejectReason);
                if (committed)
                {
                    _lastBefore = before;
                    _lastAfter = after;
                }
            }
            if (!committed)
            {
                // Accepted but refused at the final managed gate: keep the ticket pending/drainable, stamp
                // nothing, and admit the pure-managed drain entry before returning false.
                RequestStop(generation);
                reason = (commitStopCancel ? "NC02_STAGING_SUBMIT_STOPPED_POST_ACCEPT" : commitRejectReason) + "|pairId=" + FormatInvariant(acceptedPairId) + "|drainRequired=1";
                return false;
            }
            pairId = acceptedPairId;
            return true;
        }

        // ------------------------------------------------------------------
        // Source format reading: standard IDirect3DSurface9::GetDesc via vtable slot 12
        // (HRESULT __stdcall (this, out D3DSURFACE_DESC)), pack 4 / sizeof 32. The local delegate and the
        // borrowed surface pointer are used ONLY inside this stack frame: no AddRef, no Release, no COM rebuild
        // from a numeric identity, and the actual Format value is what later reaches Create (never 21-then-guess-22).
        // ------------------------------------------------------------------
        private static NC02D3D9SurfaceDescV1 RequireValidatedStagingSurfaceDesc(int eye, IntPtr surface, int expectedWidth, int expectedHeight)
        {
            if (surface == IntPtr.Zero) throw new InvalidOperationException("NC02_STAGING_SURFACE_NULL:eye=" + FormatInvariant(eye));
            IntPtr vtable = Marshal.ReadIntPtr(surface);
            if (vtable == IntPtr.Zero) throw new InvalidOperationException("NC02_STAGING_VTABLE_NULL:eye=" + FormatInvariant(eye));
            IntPtr getDescSlot = Marshal.ReadIntPtr(vtable, Nc02SurfaceGetDescVtableSlot * IntPtr.Size);
            if (getDescSlot == IntPtr.Zero) throw new InvalidOperationException("NC02_STAGING_GETDESC_SLOT_NULL:eye=" + FormatInvariant(eye) + "|slot=" + FormatInvariant(Nc02SurfaceGetDescVtableSlot));
            NC02SurfaceGetDescVtable getDesc = (NC02SurfaceGetDescVtable)Marshal.GetDelegateForFunctionPointer(getDescSlot, typeof(NC02SurfaceGetDescVtable));
            NC02D3D9SurfaceDescV1 desc;
            int hr;
            try { hr = getDesc(surface, out desc); }
            catch (Exception ex) { throw new InvalidOperationException("NC02_STAGING_GETDESC_INVOKE_FAILED:eye=" + FormatInvariant(eye) + "|actualtype=" + ex.GetType().FullName, ex); }
            string eyeTag = "|eye=" + FormatInvariant(eye);
            if (hr != 0) throw new InvalidOperationException("NC02_STAGING_GETDESC_FAILED:hr=0x" + hr.ToString("x8", CultureInfo.InvariantCulture) + eyeTag);
            if (desc.Format != Nc02FormatA8R8G8B8 && desc.Format != Nc02FormatX8R8G8B8)
                throw new InvalidOperationException("NC02_STAGING_FORMAT_UNSUPPORTED:actual=" + FormatInvariant(desc.Format) + "|expected=21-A8R8G8B8-or-22-X8R8G8B8" + eyeTag);
            if (desc.Pool != 0u) throw new InvalidOperationException("NC02_STAGING_POOL_NOT_DEFAULT:actual=" + FormatInvariant(desc.Pool) + eyeTag);
            if (desc.Type != 1u) throw new InvalidOperationException("NC02_STAGING_TYPE_NOT_SURFACE:actual=" + FormatInvariant(desc.Type) + eyeTag);
            if (desc.Usage != 1u) throw new InvalidOperationException("NC02_STAGING_USAGE_NOT_RENDERTARGET:actual=" + FormatInvariant(desc.Usage) + eyeTag);
            if (desc.MultiSampleType != 0u || desc.MultiSampleQuality != 0u)
                throw new InvalidOperationException("NC02_STAGING_MSAA_UNSUPPORTED:msType=" + FormatInvariant(desc.MultiSampleType) + "|msQuality=" + FormatInvariant(desc.MultiSampleQuality) + eyeTag);
            if (desc.Width != (uint)expectedWidth || desc.Height != (uint)expectedHeight)
                throw new InvalidOperationException("NC02_STAGING_SURFACE_SIZE_MISMATCH:actual=" + FormatInvariant(desc.Width) + "x" + FormatInvariant(desc.Height)
                    + "|expected=" + FormatInvariant(expectedWidth) + "x" + FormatInvariant(expectedHeight) + eyeTag);
            return desc;
        }

        private static string HexPointer(IntPtr value)
        {
            return value.ToInt64().ToString("x16", CultureInfo.InvariantCulture);
        }

        private static string DescribeStagingFault(Exception fault)
        {
            if (fault == null) return "ok";
            return "type=" + fault.GetType().FullName + "|message=" + fault.Message;
        }

        // ------------------------------------------------------------------
        // Independent NC02 module gate: fixed absolute candidate path, own lock/flag (the legacy NC01
        // constructor-time EnsureHelperLoaded is untouched and the legacy synchronous API keeps its helper).
        // Same-basename already mapped from a foreign path => refuse; no unload, no replacement, no retry.
        // ------------------------------------------------------------------
        private static void EnsureNc02HelperLoaded()
        {
            lock (Nc02ModuleLock)
            {
                if (_nc02ModuleChecked) return;
                VerifyNc02AbiLayout();
#if VRSIM_NATIVE_QA
                string full = Path.GetFullPath(Nc02HelperDllAbsolutePath);
#else
                string nc02ManagedAssemblyLocation = typeof(NC01HostedSceneOwner).Assembly.Location;
                if (string.IsNullOrEmpty(nc02ManagedAssemblyLocation) || !Path.IsPathRooted(nc02ManagedAssemblyLocation))
                {
                    throw new InvalidOperationException("NC02_ASSEMBLY_LOCATION_UNRESOLVED");
                }
                string nc02ManagedAssemblyDirectory = Path.GetDirectoryName(nc02ManagedAssemblyLocation);
                if (string.IsNullOrEmpty(nc02ManagedAssemblyDirectory))
                {
                    throw new InvalidOperationException("NC02_ASSEMBLY_DIRECTORY_UNRESOLVED");
                }
                string full = Path.GetFullPath(Path.Combine(nc02ManagedAssemblyDirectory, Nc02HelperDllFileName));
#endif
                if (!string.Equals(Path.GetFileName(full), Nc02HelperDllFileName, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("NC02_DLL_NAME_MISMATCH");
                if (!File.Exists(full)) throw new FileNotFoundException("NC02_DLL_NOT_FOUND:" + full, full);
                IntPtr existing = GetModuleHandleW(Nc02HelperDllFileName);
                if (existing != IntPtr.Zero)
                {
                    if (!ModulePathMatches(existing, full)) throw new InvalidOperationException("NC02_MODULE_ALIAS_CONFLICT:mapped=" + FormatInvariant(GetNc02ModulePathForDiagnostic(existing)) + "|intended=" + full);
                }
                else
                {
                    IntPtr loaded = LoadLibraryW(full);
                    if (loaded == IntPtr.Zero) throw new InvalidOperationException("NC02_MODULE_LOAD_FAILED:lasterror=" + FormatInvariant(Marshal.GetLastWin32Error()));
                    if (!ModulePathMatches(loaded, full)) { FreeLibrary(loaded); throw new InvalidOperationException("NC02_MODULE_ALIAS_CONFLICT:loaded=" + FormatInvariant(GetNc02ModulePathForDiagnostic(loaded)) + "|intended=" + full); }
                }
                _nc02ModuleChecked = true;
            }
        }

        private static string GetNc02ModulePathForDiagnostic(IntPtr module)
        {
            StringBuilder buffer = new StringBuilder(1024);
            uint length = GetModuleFileNameW(module, buffer, (uint)buffer.Capacity);
            return length == 0u ? "<unreadable>" : buffer.ToString();
        }

        private static void ThrowNc02AbiMismatch(string what, uint expected, uint actual)
        {
            throw new InvalidOperationException("NC02_ABI_MISMATCH:" + what + "|expected=" + FormatInvariant(expected) + "|actual=" + FormatInvariant(actual));
        }

        private static uint OffsetOfStaging(Type type, string field)
        {
            return (uint)Marshal.OffsetOf(type, field).ToInt32();
        }

        // Marshal.SizeOf + critical Marshal.OffsetOf checks against the 176B/56B/40B header contract BEFORE the
        // first Create; the legacy 80B/32B NC01-era shapes must never reach this path.
        private static void VerifyNc02AbiLayout()
        {
            if ((uint)Marshal.SizeOf(typeof(NC02PairTokenV1)) != 40u) ThrowNc02AbiMismatch("token-size", 40u, (uint)Marshal.SizeOf(typeof(NC02PairTokenV1)));
            if ((uint)Marshal.SizeOf(typeof(NC02CollectedPairV1)) != 176u) ThrowNc02AbiMismatch("collected-size", 176u, (uint)Marshal.SizeOf(typeof(NC02CollectedPairV1)));
            if ((uint)Marshal.SizeOf(typeof(NC02StopStateV1)) != 56u) ThrowNc02AbiMismatch("stop-size", 56u, (uint)Marshal.SizeOf(typeof(NC02StopStateV1)));
            if ((uint)Marshal.SizeOf(typeof(NC02D3D9SurfaceDescV1)) != 32u) ThrowNc02AbiMismatch("d3d-desc-size", 32u, (uint)Marshal.SizeOf(typeof(NC02D3D9SurfaceDescV1)));
            Type token = typeof(NC02PairTokenV1);
            if (OffsetOfStaging(token, "StructBytes") != 0u || OffsetOfStaging(token, "Version") != 4u
                || OffsetOfStaging(token, "PairSeq") != 8u || OffsetOfStaging(token, "PoseId") != 16u
                || OffsetOfStaging(token, "Epoch") != 24u || OffsetOfStaging(token, "OwnerGeneration") != 32u)
                throw new InvalidOperationException("NC02_ABI_MISMATCH:token-offsets");
            Type collected = typeof(NC02CollectedPairV1);
            if (OffsetOfStaging(collected, "token") != 0u || OffsetOfStaging(collected, "slotIndex") != 40u
                || OffsetOfStaging(collected, "status") != 44u || OffsetOfStaging(collected, "leftSourceIdentity") != 48u
                || OffsetOfStaging(collected, "rightSourceIdentity") != 56u || OffsetOfStaging(collected, "deviceIdentity") != 64u
                || OffsetOfStaging(collected, "width") != 72u || OffsetOfStaging(collected, "height") != 76u
                || OffsetOfStaging(collected, "submitQpc") != 80u || OffsetOfStaging(collected, "collectQpc") != 88u
                || OffsetOfStaging(collected, "getRenderTargetDataTicks") != 96u || OffsetOfStaging(collected, "issueTicks") != 104u
                || OffsetOfStaging(collected, "queryTicks") != 112u || OffsetOfStaging(collected, "lockTicks") != 120u
                || OffsetOfStaging(collected, "rowCopyTicks") != 128u || OffsetOfStaging(collected, "unlockTicks") != 136u
                || OffsetOfStaging(collected, "submitTotalTicks") != 144u || OffsetOfStaging(collected, "pollTotalTicks") != 152u
                || OffsetOfStaging(collected, "queryAttempts") != 160u || OffsetOfStaging(collected, "leftLockAttempts") != 164u
                || OffsetOfStaging(collected, "rightLockAttempts") != 168u || OffsetOfStaging(collected, "reserved") != 172u)
                throw new InvalidOperationException("NC02_ABI_MISMATCH:collected-offsets");
            Type stop = typeof(NC02StopStateV1);
            if (OffsetOfStaging(stop, "structBytes") != 0u || OffsetOfStaging(stop, "version") != 4u
                || OffsetOfStaging(stop, "admissionOpen") != 8u || OffsetOfStaging(stop, "callBusy") != 12u
                || OffsetOfStaging(stop, "pendingSlots") != 16u || OffsetOfStaging(stop, "liveOwnedResourceRefs") != 20u
                || OffsetOfStaging(stop, "stopVerified") != 24u || OffsetOfStaging(stop, "faulted") != 28u
                || OffsetOfStaging(stop, "lastError") != 32u || OffsetOfStaging(stop, "reserved") != 36u
                || OffsetOfStaging(stop, "createTicks") != 40u || OffsetOfStaging(stop, "releaseTicks") != 48u)
                throw new InvalidOperationException("NC02_ABI_MISMATCH:stop-offsets");
            Type desc = typeof(NC02D3D9SurfaceDescV1);
            if (OffsetOfStaging(desc, "Format") != 0u || OffsetOfStaging(desc, "Type") != 4u
                || OffsetOfStaging(desc, "Usage") != 8u || OffsetOfStaging(desc, "Pool") != 12u
                || OffsetOfStaging(desc, "MultiSampleType") != 16u || OffsetOfStaging(desc, "MultiSampleQuality") != 20u
                || OffsetOfStaging(desc, "Width") != 24u || OffsetOfStaging(desc, "Height") != 28u)
                throw new InvalidOperationException("NC02_ABI_MISMATCH:d3d-desc-offsets");
        }

        // ------------------------------------------------------------------
        // TryCollectStagingRawPair: UI-thread, bound generation, admission OPEN, no stop, no other busy; a
        // managed-empty queue is a plain false (NOT a fault). At most ONE native PollCollect per call, run
        // OUTSIDE the gate lock. A completed turn never re-reads camera/scene/main/snapshot — only the frozen
        // ticket plus this call's native 176B struct are published.
        // ------------------------------------------------------------------
        public bool TryCollectStagingRawPair(int generation, out NC02SourcePairResultV1 completed, out string reason)
        {
            completed = null; reason = null;
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            if (!_runtimeBackend) throw new InvalidOperationException("NC02_STAGING_RUNTIME_BACKEND_REQUIRED:collect");
            VerifyAccess();
            NC02StagingTicket oldest = null;
            ulong handle = 0UL;
            bool notFrozen = false;
            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) throw new InvalidOperationException("NC02_STAGING_COLLECT_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + FormatInvariant(generation));
                if (Volatile.Read(ref _stopRequested) != 0) { reason = "NC02_STAGING_COLLECT_STOP_REQUESTED:generation=" + FormatInvariant(generation); return false; }
                if (Volatile.Read(ref _admissionOpen) == 0) { reason = "NC02_STAGING_COLLECT_ADMISSION_CLOSED:generation=" + FormatInvariant(generation); return false; }
                if (_captureDisabled) { reason = "NC02_STAGING_COLLECT_CAPTURE_DISABLED:" + _disabledReason; return false; }
                if (Volatile.Read(ref _hostingState) != 2) { reason = "NC02_STAGING_COLLECT_NOT_HOSTED:hostingState=" + FormatInvariant(Volatile.Read(ref _hostingState)); return false; }
                if (Volatile.Read(ref _stagingCallBusy) != 0 || Volatile.Read(ref _runtimeCallsBusy) != 0 || Volatile.Read(ref _captureBusy) != 0)
                { reason = "NC02_STAGING_COLLECT_CALL_BUSY:generation=" + FormatInvariant(generation); return false; }
                if (Volatile.Read(ref _stagingPendingCount) == 0) { reason = "NC02_STAGING_COLLECT_PENDING_EMPTY:generation=" + FormatInvariant(generation) + "|fault=0"; return false; }
                handle = _stagingHelperHandle;
                oldest = _stagingTickets[_stagingTicketHead];
                if (handle == 0UL || oldest == null) { reason = "NC02_STAGING_COLLECT_SESSION_INCONSISTENT:handle=" + FormatInvariant(handle) + "|pending=" + FormatInvariant(Volatile.Read(ref _stagingPendingCount)); return false; }
                if (!oldest.FinalFrozen)
                {
                    // An unfrozen ticket (finalize never completed post-accept) must never be polled or published:
                    // stop below and drain with the ticket left pending.
                    notFrozen = true;
                }
                else
                {
                    Volatile.Write(ref _stagingCallBusy, 1);
                    _runtimeCallsBusy = 1;
                }
            }
            if (notFrozen)
            {
                RequestStop(generation);
                reason = "NC02_STAGING_COLLECT_TICKET_NOT_FROZEN:pairId=" + FormatInvariant(oldest.PairId) + "|drainRequired=1";
                return false;
            }
            // The full busy window (_stagingCallBusy + _runtimeCallsBusy) is held across the ENTIRE turn —
            // NativePoll, token verification, immutable result construction and the atomic commit/discard —
            // because the publish grant refuses while any busy flag is already clear. The outer finally below
            // is the ONLY place these flags return to 0, so every Pending/fault/exception path stays admitted.
            try
            {
                int pollRc;
                NC02CollectedPairV1 collected = default(NC02CollectedPairV1);
                Stopwatch pollSw = Stopwatch.StartNew();
                try
                {
                    pollRc = NC02_PairStage_PollCollectV1(handle, oldest.LeftBgra, (ulong)oldest.LeftBgra.LongLength,
                        oldest.RightBgra, (ulong)oldest.RightBgra.LongLength, ref collected, 176u);
                }
                finally
                {
                    // Inner finally: timer bookkeeping ONLY; the busy restore lives in the outer finally.
                    pollSw.Stop();
                    oldest.CollectNativeMs += pollSw.Elapsed.TotalMilliseconds;
                }
                if (pollRc == 1) { reason = "NC02_STAGING_COLLECT_PENDING:pairId=" + FormatInvariant(oldest.PairId) + "|pollMs=" + FormatInvariant(pollSw.Elapsed.TotalMilliseconds); return false; }
                if (pollRc < 0)
                {
                    // Native poll fault: stop new capture/publish and preserve the session for TryDrainStagingStop.
                    // Never Destroy here; tickets and drained=false stay honest for the drain flow.
                    RequestStop(generation);
                    reason = "NC02_STAGING_COLLECT_NATIVE_FAULT:rc=" + FormatInvariant(pollRc) + "|pairId=" + FormatInvariant(oldest.PairId) + "|drainRequired=1";
                    return false;
                }
                string mismatch = DescribeStagingTokenMismatch(oldest, collected, generation);
                if (mismatch != null)
                {
                    // Wrong/expired completion: refuse the WHOLE pair and stop. The native slot was already consumed
                    // by this Complete result, so only the managed ticket metadata is discarded.
                    RequestStop(generation);
                    bool discarded;
                    lock (_gateLock) { discarded = DiscardOldestStagingTicketLocked(oldest); }
                    reason = "NC02_STAGING_COLLECT_" + mismatch + "|pairId=" + FormatInvariant(oldest.PairId) + "|discarded=" + Bool(discarded) + "|drainRequired=1";
                    return false;
                }
                // The immutable result (including the exact 176B raw serialization of THIS collected struct) is
                // constructed OUTSIDE the gate lock but INSIDE the full busy window, BEFORE any ticket is touched:
                // an OOM here leaves the ticket pending for the drain flow instead of a not-published return
                // over a dropped ticket. No extra SDK/native query runs on this path.
                NC02SourcePairResultV1 result = new NC02SourcePairResultV1(oldest, collected);
                bool published = false;
                lock (_gateLock)
                {
                    bool stopCancel;
                    string rejectReason;
                    if (RuntimeGroupGrant(generation, "NC02_STAGING_COLLECT_COMMIT", true, out stopCancel, out rejectReason)
                        && Volatile.Read(ref _stagingPendingCount) > 0
                        && ReferenceEquals(_stagingTickets[_stagingTicketHead], oldest))
                    {
                        _stagingTickets[_stagingTicketHead] = null;
                        _stagingTicketHead = (_stagingTicketHead + 1) % Nc02MaxPendingSlots;
                        Volatile.Write(ref _stagingPendingCount, Volatile.Read(ref _stagingPendingCount) - 1);
                        published = true;
                    }
                }
                if (!published)
                {
                    // Stop/fault landed after the native copy: the Complete slot is ALREADY safely freed, so the
                    // frozen metadata is discarded (pending updated honestly) and the result is not published.
                    bool discarded;
                    lock (_gateLock) { discarded = DiscardOldestStagingTicketLocked(oldest); }
                    reason = "NC02_STAGING_COLLECT_NOT_PUBLISHED:pairId=" + FormatInvariant(oldest.PairId) + "|slotFreed=1|discarded=" + Bool(discarded)
                        + "|remainingPending=" + FormatInvariant(Volatile.Read(ref _stagingPendingCount)) + "|drainRequired=" + Bool(Volatile.Read(ref _stagingPendingCount) > 0) + "|publishGate=0";
                    return false;
                }
                completed = result;
                return true;
            }
            catch
            {
                // Unexpected mid-collect failure (OOM included): admit the drain entry, keep the session and the
                // tickets exactly as they are, and NEVER Destroy on this path.
                RequestStop(generation);
                throw;
            }
            finally
            {
                lock (_gateLock) { Volatile.Write(ref _runtimeCallsBusy, 0); Volatile.Write(ref _stagingCallBusy, 0); }
            }
        }

        private static string DescribeStagingTokenMismatch(NC02StagingTicket ticket, NC02CollectedPairV1 collected, int generation)
        {
            if (collected.token.StructBytes != 40u || collected.token.Version != 1u)
                return "TOKEN_ABI_INVALID:structBytes=" + FormatInvariant(collected.token.StructBytes) + "|version=" + FormatInvariant(collected.token.Version);
            if (collected.token.PairSeq != ticket.PairId) return "TOKEN_SEQ_MISMATCH:expected=" + FormatInvariant(ticket.PairId) + "|actual=" + FormatInvariant(collected.token.PairSeq);
            // Every identity comparison below uses THIS old ticket's frozen submitted values — never a reused
            // constant, never a fresh camera/main/XR/current-pose read, and never the owner's live session epoch.
            if (collected.token.PoseId != ticket.SubmittedPoseId) return "TOKEN_POSE_MISMATCH:expected=" + FormatInvariant(ticket.SubmittedPoseId) + "|actual=" + FormatInvariant(collected.token.PoseId);
            if (collected.token.Epoch != ticket.SubmittedEpoch) return "TOKEN_EPOCH_MISMATCH:expected=" + FormatInvariant(ticket.SubmittedEpoch) + "|actual=" + FormatInvariant(collected.token.Epoch);
            if (generation != ticket.Generation || collected.token.OwnerGeneration != (ulong)ticket.Generation)
                return "TOKEN_GENERATION_MISMATCH:expected=" + FormatInvariant(ticket.Generation) + "|actual=" + FormatInvariant(collected.token.OwnerGeneration) + "|argGen=" + FormatInvariant(generation);
            NC02XrCaptureIdentityV1 mismatchIdentity = ticket.CaptureIdentity;
            if (mismatchIdentity != null)
            {
                if (mismatchIdentity.FrameSerial == 0UL || mismatchIdentity.PoseSerial == 0UL || mismatchIdentity.NativeXrEpoch == 0UL)
                    return "TICKET_IDENTITY_NONZERO_VIOLATION:frameSerial=" + FormatInvariant(mismatchIdentity.FrameSerial)
                        + "|poseSerial=" + FormatInvariant(mismatchIdentity.PoseSerial) + "|nativeXrEpoch=" + FormatInvariant(mismatchIdentity.NativeXrEpoch);
                if (mismatchIdentity.OriginalGeneration != ticket.Generation || mismatchIdentity.PoseSerial != ticket.SubmittedPoseId
                    || mismatchIdentity.NativeXrEpoch != ticket.SubmittedEpoch)
                    return "TICKET_IDENTITY_INTERNAL_MISMATCH:identityGen=" + FormatInvariant(mismatchIdentity.OriginalGeneration)
                        + "|ticketGen=" + FormatInvariant(ticket.Generation)
                        + "|identityPose=" + FormatInvariant(mismatchIdentity.PoseSerial) + "|submittedPose=" + FormatInvariant(ticket.SubmittedPoseId)
                        + "|identityEpoch=" + FormatInvariant(mismatchIdentity.NativeXrEpoch) + "|submittedEpoch=" + FormatInvariant(ticket.SubmittedEpoch);
            }
            else
            {
                if (ticket.SubmittedPoseId != 0UL || ticket.SubmittedEpoch != Nc02SourceLeaseEpoch)
                    return "TOKEN_STATIC_EPOCH_POSE_VIOLATION:poseId=" + FormatInvariant(ticket.SubmittedPoseId) + "|epoch=" + FormatInvariant(ticket.SubmittedEpoch);
            }
            if (collected.status != 0u) return "STATUS_NOT_COMPLETE:status=" + FormatInvariant(collected.status);
            if (collected.width != (uint)ticket.Width || collected.height != (uint)ticket.Height)
                return "SHAPE_MISMATCH:expected=" + FormatInvariant(ticket.Width) + "x" + FormatInvariant(ticket.Height) + "|actual=" + FormatInvariant(collected.width) + "x" + FormatInvariant(collected.height);
            if (collected.leftSourceIdentity != ticket.LeftSourceIdentity || collected.rightSourceIdentity != ticket.RightSourceIdentity)
                return "SOURCE_IDENTITY_MISMATCH:expectedL=" + FormatInvariant(ticket.LeftSourceIdentity) + "|actualL=" + FormatInvariant(collected.leftSourceIdentity)
                    + "|expectedR=" + FormatInvariant(ticket.RightSourceIdentity) + "|actualR=" + FormatInvariant(collected.rightSourceIdentity);
            if (collected.deviceIdentity == 0UL) return "DEVICE_IDENTITY_ZERO";
            return null;
        }

        // Caller must already hold _gateLock; UI-thread staging ownership makes head/tail single-writer.
        private bool DiscardOldestStagingTicketLocked(NC02StagingTicket ticket)
        {
            if (Volatile.Read(ref _stagingPendingCount) <= 0) return false;
            if (!ReferenceEquals(_stagingTickets[_stagingTicketHead], ticket)) return false;
            _stagingTickets[_stagingTicketHead] = null;
            _stagingTicketHead = (_stagingTicketHead + 1) % Nc02MaxPendingSlots;
            Volatile.Write(ref _stagingPendingCount, Volatile.Read(ref _stagingPendingCount) - 1);
            return true;
        }

        // ------------------------------------------------------------------
        // TryDrainStagingStop: the ONLY entry allowed to touch the native session after a stop; it performs NO
        // SDK/source-camera reads. Gates: original UI thread, original bound generation, stopRequested, no
        // other call busy — a closed admission is explicitly allowed for cleanup. At most ONE StopStep per
        // call, NO Sleep/while loop here (the 32-step/500ms deadline belongs to the Codex QA scheduler).
        // ------------------------------------------------------------------
        public bool TryDrainStagingStop(int generation, out string reason)
        {
            reason = null;
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            if (!_runtimeBackend) throw new InvalidOperationException("NC02_STAGING_RUNTIME_BACKEND_REQUIRED:drain");
            VerifyAccess();
            ulong handle = 0UL;
            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) throw new InvalidOperationException("NC02_STAGING_DRAIN_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + FormatInvariant(generation));
                if (Volatile.Read(ref _stopRequested) == 0) { reason = "NC02_STAGING_DRAIN_STOP_NOT_REQUESTED:generation=" + FormatInvariant(generation); return false; }
                if (Volatile.Read(ref _stagingCallBusy) != 0 || Volatile.Read(ref _runtimeCallsBusy) != 0 || Volatile.Read(ref _captureBusy) != 0)
                { reason = "NC02_STAGING_DRAIN_CALL_BUSY:generation=" + FormatInvariant(generation); return false; }
                if (_stagingHelperDrained && Volatile.Read(ref _stagingPendingCount) == 0 && _stagingHelperHandle == 0UL)
                { reason = "NC02_STAGING_DRAIN_ALREADY_CLEAN:generation=" + FormatInvariant(generation); return true; }
                handle = _stagingHelperHandle;
                if (handle == 0UL)
                {
                    // pending>0 without a live session can never be drained safely: report honestly, keep state.
                    reason = "NC02_STAGING_DRAIN_SESSION_INCONSISTENT:handle=0|pending=" + FormatInvariant(Volatile.Read(ref _stagingPendingCount)) + "|drained=" + Bool(_stagingHelperDrained);
                    return false;
                }
                Volatile.Write(ref _stagingCallBusy, 1);
                _runtimeCallsBusy = 1;
            }
            // Once the busy registration above succeeded, the ENTIRE drain body — native StopStep, Destroy and the
            // managed bookkeeping — runs inside one outer try/finally that restores BOTH _runtimeCallsBusy and
            // _stagingCallBusy on every return, fault and exception path. Leaving them at 1 after a Pending round
            // would brick every later collect/stop/drain retry forever. No SDK/scene/camera read on this path.
            try
            {
                NC02StopStateV1 state = default(NC02StopStateV1);
                int stepRc;
                Stopwatch stepSw = Stopwatch.StartNew();
                try { stepRc = NC02_PairStage_StopStepV1(handle, ref state, 56u); }
                finally { stepSw.Stop(); }
                if (stepRc == 1)
                {
                    // Drain Pending: native keeps handle/refs; managed tickets and drained=false stay honest.
                    reason = "NC02_STAGING_DRAIN_PENDING:stepMs=" + FormatInvariant(stepSw.Elapsed.TotalMilliseconds)
                        + "|pendingSlots=" + FormatInvariant(state.pendingSlots) + "|callBusy=" + FormatInvariant(state.callBusy)
                        + "|refs=" + FormatInvariant(state.liveOwnedResourceRefs) + "|managedPending=" + FormatInvariant(Volatile.Read(ref _stagingPendingCount));
                    return false;
                }
                if (stepRc < 0)
                {
                    // Native fault: keep handle/tickets/drained=false for a later drain attempt; never pretend this
                    // round verified a safe stop (a faulted context can still Stop safely later — that then returns true).
                    reason = "NC02_STAGING_DRAIN_NATIVE_FAULT:rc=" + FormatInvariant(stepRc) + "|handleKept=" + FormatInvariant(handle) + "|drained=0";
                    return false;
                }
                // stepRc == 0: verify the FULL 56B proof before Destroy. stop56.callBusy is native's OWN view of
                // its gate (normally 0) — the managed busy flags belong to THIS thread — so the proof must also
                // show native admission closed (admissionOpen=0) and the reserved word still zero.
                bool stepVerified = state.structBytes == 56u && state.version == 1u && state.stopVerified == 1u
                    && state.liveOwnedResourceRefs == 0u && state.pendingSlots == 0u && state.callBusy == 0u
                    && state.admissionOpen == 0u && state.reserved == 0u;
                if (!stepVerified)
                {
                    reason = "NC02_STAGING_DRAIN_STEP_ZERO_UNVERIFIED:structBytes=" + FormatInvariant(state.structBytes) + "|version=" + FormatInvariant(state.version)
                        + "|stopVerified=" + FormatInvariant(state.stopVerified) + "|refs=" + FormatInvariant(state.liveOwnedResourceRefs)
                        + "|pendingSlots=" + FormatInvariant(state.pendingSlots) + "|callBusy=" + FormatInvariant(state.callBusy) + "|faulted=" + FormatInvariant(state.faulted)
                        + "|admissionOpen=" + FormatInvariant(state.admissionOpen) + "|reserved=" + FormatInvariant(state.reserved)
                        + "|lastError=" + FormatInvariant(state.lastError);
                    return false;
                }
                int destroyRc = NC02_PairStage_DestroyV1(handle);
                if (destroyRc != 0)
                {
                    reason = "NC02_STAGING_DRAIN_DESTROY_FAILED:rc=" + FormatInvariant(destroyRc) + "|handleKept=" + FormatInvariant(handle) + "|drained=0";
                    return false;
                }
                lock (_gateLock)
                {
                    _stagingHelperHandle = 0UL;
                    _stagingTickets[0] = null;
                    _stagingTickets[1] = null;
                    _stagingTicketHead = 0;
                    _stagingTicketTail = 0;
                    Volatile.Write(ref _stagingPendingCount, 0);
                    // Only now — Destroy returned 0 on a fully verified stop — may the drained flag flip and
                    // the cached session identity clear. Pending/fault/native-busy paths above kept handle +
                    // drained=false on purpose; _stopRequested stays permanently closed for this owner.
                    _stagingSessionEpoch = 0UL;
                    _stagingSessionUsesXrIdentity = false;
                    _stagingLastAcceptedXrFrameSerial = 0UL;
                    _stagingHelperDrained = true;
                }
                reason = "NC02_STAGING_DRAIN_COMPLETE:stopStepMs=" + FormatInvariant(stepSw.Elapsed.TotalMilliseconds)
                    + "|createTicks=" + FormatInvariant(state.createTicks) + "|releaseTicks=" + FormatInvariant(state.releaseTicks)
                    + "|lastError=" + FormatInvariant(state.lastError) + "|faultedEvidence=" + FormatInvariant(state.faulted);
                return true;
            }
            finally
            {
                lock (_gateLock) { Volatile.Write(ref _runtimeCallsBusy, 0); Volatile.Write(ref _stagingCallBusy, 0); }
            }
        }
    }
}
