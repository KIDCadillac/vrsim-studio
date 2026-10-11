using System;
using System.Diagnostics;
using System.Globalization;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using VisualComponents.Create3D;
using Matrix = VisualComponents.Create3D.Matrix;

namespace NC02XrQA
{
    // Successful whole-pair GPU-shared PREPARE outcome. Returned ONLY after the atomic managed commit AND after both
    // D3DImage Unlocks have succeeded; a refused, cancelled or faulted transaction never produces one. The ulong
    // PreparedTicket is the raw synchronous callback result — a non-zero return does NOT prove a native PASS; rc
    // interpretation belongs to the root caller. The caller producer owns the typed native packet inside its closure:
    // it MUST DISCARD the returned PreparedTicket if this method throws or any post-guard fails, and MUST re-check the
    // original running/admission/epoch/generation before committing; a ticket is committed ONLY after this method
    // returns normally AND that re-check still passes.
    public sealed class NC01GpuPreparedPairV1
    {
        public ulong PreparedTicket;
        public int Generation;
        public int Width;
        public int Height;
        public double[] LeftActualView;
        public double[] RightActualView;
        public int LeftRenderRc;
        public int RightRenderRc;
        public string LeftSurfaceHex;
        public string RightSurfaceHex;
        public string MainBefore;
        public string MainAfter;
        public bool MainPreserved;
        public bool LeftLocked;
        public bool RightLocked;
        public bool LeftUnlocked;
        public bool RightUnlocked;
        public double LeftRenderMs;
        public double RightRenderMs;
        public double ProbeMs;
        public double TotalMs;
    }

// OWNER_PHASE_COMPILE_GATE_BEGIN
#if VRSIM_QA_OWNER_PHASE_CPU_WALL && (!VRSIM_QA_OUTPUT_4096 || !VRSIM_QA_D9_BEGIN_POLL)
#error "VRSIM_QA_OWNER_PHASE_CPU_WALL requires VRSIM_QA_OUTPUT_4096 and VRSIM_QA_D9_BEGIN_POLL"
#endif
// OWNER_PHASE_COMPILE_GATE_END

#if VRSIM_QA_D9_BEGIN_POLL
#if !VRSIM_NATIVE_QA
#error "VRSIM_QA_D9_BEGIN_POLL requires VRSIM_NATIVE_QA"
#endif
#if !NC02_XR_QA
#error "VRSIM_QA_D9_BEGIN_POLL requires NC02_XR_QA"
#endif
#if !VRSIM_QA_MIXED_SOURCE
#error "VRSIM_QA_D9_BEGIN_POLL requires VRSIM_QA_MIXED_SOURCE"
#endif
#if !VRSIM_SHARED_3072
#error "VRSIM_QA_D9_BEGIN_POLL requires VRSIM_SHARED_3072"
#endif
#if !VRSIM_SOURCE_4096_QA
#error "VRSIM_QA_D9_BEGIN_POLL requires VRSIM_SOURCE_4096_QA"
#endif
    public struct NC01GpuSplitStampV1
    {
        public ulong FrameSerial;
        public ulong PoseSerial;
        public ulong XrEpoch;
        public int Generation;
        public long PoseSampleBeginQpc;
        public long PoseSampleEndQpc;
        public long CaptureBeginQpc;
    }
    public struct NC01GpuSplitTokenV1
    {
        public uint Version;
        public uint StructBytes;
        public uint SlotIndex;
        public uint Generation;
        public ulong Epoch;
        public ulong Nonce;
    }
    public struct NC01GpuSplitNativeReplyV1
    {
        public int NativeRc;
        public uint ResultVersion;
        public uint ResultStructBytes;
        public uint Status;
        public uint LedgerStateAfter;
        public int BridgeHresult;
        public int FirstFailureHr;
        public int LastIssueHr;
        public int LastGetDataHr;
        public uint CopyExecMask;
        public uint CopyOkMask;
        public uint FaultConsumed;
        public uint GetDataCallsThisCall;
        public uint TicketGranted;
        public ulong ElapsedMs;
        public ulong GetDataCallsTotal;
        public NC01GpuSplitTokenV1 Token;
        public uint TicketVersion;
        public uint TicketStructBytes;
        public uint TicketSlotIndex;
        public uint TicketGeneration;
        public ulong TicketEpoch;
        public ulong PreparedTicket;
        public ulong LegacyLeft;
        public ulong LegacyRight;
        public uint TicketWidth;
        public uint TicketHeight;
        public uint TicketFormat88;
        public uint TicketReserved;
        public uint ReadbackShapeValid;
    }
    public struct NC01GpuSplitStepV1
    {
        public uint State;
        public uint LastOperationKind;
        public NC01GpuSplitStampV1 Stamp;
        public NC01GpuSplitTokenV1 Token;
        public NC01GpuSplitNativeReplyV1 LastReply;
        public uint LockGrantedMask;
        public uint UnlockedMask;
        public uint CallBusy;
        public uint LogicalBusy;
    }
#endif

    public sealed partial class NC01HostedSceneOwner
    {
        // Owner-synchronized shared-pair GPU PREPARE seam: renders BOTH eyes under their D3DImage locks and hands the
        // two frozen IDirect3DSurface9 pointers to ONE synchronous native PREPARE callback on the UI thread. The scene
        // pointers exist only as locals inside the callback group; nothing is stored across calls, no Task/async/
        // background work, no CPU pixel readback, no native publish/commit, no OpenXR call, no message pump,
        // DispatcherFrame, Show, Dispose, Reset or Present. The callback receives the borrowed genuine surface pointers
        // ONLY for the duration of the local callback and returns a ulong ticket that MUST be strictly > 0; a zero
        // ticket throws and sticky-disables captures. The result is returned ONLY after the same proven post-guards and
        // the atomic owner commit AND both Unlocks succeed. If the body throws, the original bodyException is preserved
        // as inner of the AggregateException raised by the reverse double-Unlock; an Unlock failure independently
        // DisableCaptures and aggregates the body fault plus all unlock faults. The busy gate is held in the outer
        // finally across cleanup and commit. This is a bounded owner seam — it makes NO whole-frame byte copy, NO
        // false-cleanup and NO FPS claim; original input validation and group grants are unchanged.
        public NC01GpuPreparedPairV1 CapturePreparedSharedRawPair(Matrix leftRawView, Matrix rightRawView, double leftSourceFovYDeg, double rightSourceFovYDeg,
            double sourceAspect, bool perspective, int generation, Func<bool> shouldStop, Func<ulong, ulong, ulong> synchronousPrepare)
        {
            if (synchronousPrepare == null) throw new ArgumentNullException("synchronousPrepare");
            if (shouldStop == null) throw new ArgumentNullException("shouldStop");
            if (!_runtimeBackend) throw new InvalidOperationException("OBS_GPU_RUNTIME_BACKEND_REQUIRED:runtimeBackend=0");
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            // Pure input validation BEFORE any SDK state read, busy registration or sticky fault, mirroring the
            // CaptureRawPair(generation) entry: non-finite/degenerate inputs reject as plain refusals.
            if (!perspective) throw new InvalidOperationException("OBS_GPU_PERSPECTIVE_REQUIRED:perspective=0");
            RequireRawFovY(0, leftSourceFovYDeg);
            RequireRawFovY(1, rightSourceFovYDeg);
            RequireRawSourceAspect(sourceAspect);
            VerifyAccess();
            // ONE atomic gate snapshot: original-generation match, permanent stop, admission, hosting, sticky fault,
            // frozen quality and BOTH busy registrations on the same short lock — no multi-property-read race.
            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) throw new InvalidOperationException("OBS_GPU_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _stopRequested) != 0) throw new OperationCanceledException("OBS_GPU_STOP_REQUESTED:generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _admissionOpen) == 0) throw new InvalidOperationException("OBS_GPU_ADMISSION_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _hostingState) != 2) throw new InvalidOperationException("NC01_NOT_HOSTED");
                if (_captureDisabled) throw new InvalidOperationException("NC01_CAPTURE_DISABLED:" + _disabledReason);
                if (_qualityGeneration != 1) throw new InvalidOperationException("OBS_GPU_QUALITY_NOT_FROZEN:generation=" + _qualityGeneration.ToString(CultureInfo.InvariantCulture));
                if (_runtimeCallsBusy != 0) throw new InvalidOperationException("OBS_GPU_RUNTIME_CALL_BUSY:sharedRawPair|generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (_captureBusy != 0) throw new InvalidOperationException("NC01_CAPTURE_BUSY");
                _runtimeCallsBusy = 1;
                _captureBusy = 1;
            }
            try
            {
                // Matrix indexer reads happen only after access check, busy registration and this granted group; a
                // non-finite matrix rejects OUTSIDE the sticky capture-fault handler below.
                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_INPUT_MATRIX", true, "OBS_GPU_STOP_REQUESTED");
                double[] left16 = RequireRawView16Finite(0, leftRawView);
                double[] right16 = RequireRawView16Finite(1, rightRawView);
                try { return CapturePreparedSharedRawPairCore(generation, leftRawView, rightRawView, left16, right16, leftSourceFovYDeg, rightSourceFovYDeg, sourceAspect, shouldStop, synchronousPrepare); }
                catch (OperationCanceledException) { throw; }
                catch
                {
                    if (!_captureDisabled) DisableCaptures("OBS_GPU_PAIR_CAPTURE_FAULT");
                    throw;
                }
            }
            // Busy covers every dual-Unlock attempt (performed inside the core's inner finally) and is cleared here,
            // under the same gate lock, on all exit paths. shouldStop itself only runs outside the lock.
            finally { lock (_gateLock) { Volatile.Write(ref _captureBusy, 0); Volatile.Write(ref _runtimeCallsBusy, 0); } }
        }

        private NC01GpuPreparedPairV1 CapturePreparedSharedRawPairCore(int generation, Matrix leftRawView, Matrix rightRawView, double[] left16, double[] right16,
            double leftFovY, double rightFovY, double sourceAspect, Func<bool> shouldStop, Func<ulong, ulong, ulong> synchronousPrepare)
        {
            Stopwatch totalSw = Stopwatch.StartNew();

            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_PREPARE", true, "OBS_GPU_STOP_REQUESTED");
            VerifyRuntimeIdentityBindings();
            NC01MainSnapshotV1 before = MainSnapshotCore();
            string readyReason;
            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_READY0", true, "OBS_GPU_STOP_REQUESTED");
            if (!TryEyeReady(0, out readyReason) || !TryConfirmEyeStageBaseline(0, out readyReason))
                throw new InvalidOperationException("OBS_GPU_PREPARE_REFUSED:eye=0|reason=" + readyReason);
            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_READY1", true, "OBS_GPU_STOP_REQUESTED");
            if (!TryEyeReady(1, out readyReason) || !TryConfirmEyeStageBaseline(1, out readyReason))
                throw new InvalidOperationException("OBS_GPU_PREPARE_REFUSED:eye=1|reason=" + readyReason);
            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_QUALITY_BUILD", true, "OBS_GPU_STOP_REQUESTED");
            string pairFingerprint, pairBuildError;
            string[] pairSubset;
            if (!TryBuildRuntimeQualityFingerprint(before.Serialized, out pairFingerprint, out pairSubset, out pairBuildError))
            { DisableCaptures(pairBuildError); throw new InvalidOperationException(_disabledReason); }
            if (!string.Equals(_runtimeQualityFingerprint, pairFingerprint, StringComparison.Ordinal))
            { DisableCaptures("OBS_GPU_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(pairSubset)); throw new InvalidOperationException(_disabledReason); }

            // Camera configuration mirrors CaptureEyeRawForPairCore (raw WORLD-TO-CAMERA written directly, never
            // Inverse) but NEVER calls it — that core performs CPU readback. Scene pointers frozen here are the only
            // values allowed to reach the native callback.
            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_LEFT_CAMERA", true, "OBS_GPU_STOP_REQUESTED");
            EnsureEyeStagesMatchBaseline(0);
            ICamera camLeft = _eyes[0].Camera;
            camLeft.IsOrthographic = false;
            camLeft.AspectRatio = sourceAspect;
            camLeft.FieldOfViewY = leftFovY;
            camLeft.ViewMatrix = leftRawView;
            camLeft.Update();
            double[] leftActual16; double leftActualFov; double leftActualAspect; bool leftActualOrtho;
            ValidateRawEyeProjection(0, camLeft, left16, leftFovY, sourceAspect, "post-update", out leftActual16, out leftActualFov, out leftActualAspect, out leftActualOrtho);
            IntPtr leftScene = RequireScenePointer(0);

            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_RIGHT_CAMERA", true, "OBS_GPU_STOP_REQUESTED");
            EnsureEyeStagesMatchBaseline(1);
            ICamera camRight = _eyes[1].Camera;
            camRight.IsOrthographic = false;
            camRight.AspectRatio = sourceAspect;
            camRight.FieldOfViewY = rightFovY;
            camRight.ViewMatrix = rightRawView;
            camRight.Update();
            double[] rightActual16; double rightActualFov; double rightActualAspect; bool rightActualOrtho;
            ValidateRawEyeProjection(1, camRight, right16, rightFovY, sourceAspect, "post-update", out rightActual16, out rightActualFov, out rightActualAspect, out rightActualOrtho);
            IntPtr rightScene = RequireScenePointer(1);

            bool leftLocked = false, rightLocked = false, leftUnlocked = false, rightUnlocked = false;
            Exception rightUnlockFailure = null, leftUnlockFailure = null, bodyException = null;
            int leftRenderRc = 0, rightRenderRc = 0;
            double leftRenderMs = 0, rightRenderMs = 0, probeMs = 0;
            double[] leftRendered16 = null, rightRendered16 = null;
            ulong preparedTicket = 0;

            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_LEFT_LOCK", true, "OBS_GPU_STOP_REQUESTED");
            leftLocked = _images[0].TryLock(new Duration(TimeSpan.FromMilliseconds(10)));
            if (!leftLocked) throw new InvalidOperationException("OBS_GPU_DX_LOCK_TIMEOUT:eye=0");
            // From the first successful lock onward EVERY path Unlocks both eyes in reverse order (right first) in the
            // inner finally below; a right Unlock failure still attempts the left Unlock, and success bools are only
            // written on a clean return. ANY Unlock failure is a sticky fault even when the body was cancelling: the
            // finally then throws an AggregateException preserving the in-flight body exception (if any) as inner,
            // so a body OperationCanceledException can never be mistaken for a benign clean cancel.
            try
            {
                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_RIGHT_LOCK", true, "OBS_GPU_STOP_REQUESTED");
                rightLocked = _images[1].TryLock(new Duration(TimeSpan.FromMilliseconds(10)));
                if (!rightLocked) throw new InvalidOperationException("OBS_GPU_DX_LOCK_TIMEOUT:eye=1");

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_LEFT_RENDER", true, "OBS_GPU_STOP_REQUESTED");
                Stopwatch leftRenderSw = Stopwatch.StartNew();
                leftRenderRc = _app.SurfaceRender(_eyes[0]);
                leftRenderSw.Stop();
                leftRenderMs = leftRenderSw.Elapsed.TotalMilliseconds;
                if (leftRenderRc < 0) throw new InvalidOperationException("OBS_GPU_SURFACE_RENDER_FAILED:eye=0|actualrc=" + leftRenderRc.ToString(CultureInfo.InvariantCulture));
                if (RequireScenePointer(0) != leftScene) throw new InvalidOperationException("OBS_GPU_SCENE_IDENTITY_CHANGED:eye=0");
                EnsureEyeStagesMatchBaseline(0);
                ValidateRawEyeProjection(0, camLeft, left16, leftFovY, sourceAspect, "post-render", out leftRendered16, out leftActualFov, out leftActualAspect, out leftActualOrtho);

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_RIGHT_RENDER", true, "OBS_GPU_STOP_REQUESTED");
                Stopwatch rightRenderSw = Stopwatch.StartNew();
                rightRenderRc = _app.SurfaceRender(_eyes[1]);
                rightRenderSw.Stop();
                rightRenderMs = rightRenderSw.Elapsed.TotalMilliseconds;
                if (rightRenderRc < 0) throw new InvalidOperationException("OBS_GPU_SURFACE_RENDER_FAILED:eye=1|actualrc=" + rightRenderRc.ToString(CultureInfo.InvariantCulture));
                if (RequireScenePointer(1) != rightScene) throw new InvalidOperationException("OBS_GPU_SCENE_IDENTITY_CHANGED:eye=1");
                EnsureEyeStagesMatchBaseline(1);
                ValidateRawEyeProjection(1, camRight, right16, rightFovY, sourceAspect, "post-render", out rightRendered16, out rightActualFov, out rightActualAspect, out rightActualOrtho);

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_CALLBACK", true, "OBS_GPU_STOP_REQUESTED");
                if (RequireScenePointer(0) != leftScene || RequireScenePointer(1) != rightScene)
                    throw new InvalidOperationException("OBS_GPU_SCENE_IDENTITY_CHANGED_BEFORE_PROBE");
                Stopwatch probeSw = Stopwatch.StartNew();
                preparedTicket = synchronousPrepare(unchecked((ulong)leftScene.ToInt64()), unchecked((ulong)rightScene.ToInt64()));
                probeSw.Stop();
                probeMs = probeSw.Elapsed.TotalMilliseconds;
                // A non-zero ticket only proves the callback ran and returned a positive handle; it does NOT commit,
                // publish, read back or interpret native rc. strictly > 0 required; zero throws and sticky-faults.
                if (preparedTicket == 0UL) throw new InvalidOperationException("OBS_GPU_PREPARE_RETURNED_NON_POSITIVE_TICKET:ticket=0");
            }
            catch (Exception ex)
            {
                bodyException = ex;
                throw;
            }
            finally
            {
                if (rightLocked)
                {
                    try { _images[1].Unlock(); rightUnlocked = true; }
                    catch (Exception ex) { rightUnlockFailure = ex; }
                }
                if (leftLocked)
                {
                    try { _images[0].Unlock(); leftUnlocked = true; }
                    catch (Exception ex) { leftUnlockFailure = ex; }
                }
                // Pure managed sticky close plus aggregate construction — no SDK snapshot or extra SDK read happens here.
                if (rightUnlockFailure != null || leftUnlockFailure != null)
                {
                    DisableCaptures("OBS_GPU_UNLOCK_FAULT:rightLocked=" + Bool(rightLocked) + "|rightUnlocked=" + Bool(rightUnlocked)
                        + "|leftLocked=" + Bool(leftLocked) + "|leftUnlocked=" + Bool(leftUnlocked)
                        + "|rightUnlockError=" + (rightUnlockFailure != null ? rightUnlockFailure.GetType().FullName + "|originalmessage=" + rightUnlockFailure.Message : "<none>")
                        + "|leftUnlockError=" + (leftUnlockFailure != null ? leftUnlockFailure.GetType().FullName + "|originalmessage=" + leftUnlockFailure.Message : "<none>"));
                    System.Collections.Generic.List<Exception> unlockFaults = new System.Collections.Generic.List<Exception>();
                    if (bodyException != null) unlockFaults.Add(bodyException);
                    if (rightUnlockFailure != null) unlockFaults.Add(rightUnlockFailure);
                    if (leftUnlockFailure != null) unlockFaults.Add(leftUnlockFailure);
                    throw new AggregateException("OBS_GPU_UNLOCK_FAULT:unlock failure is sticky even during body cancel|originalbodytype="
                        + (bodyException != null ? bodyException.GetType().FullName : "<none>"), unlockFaults);
                }
            }

            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_FINAL_SNAPSHOT", true, "OBS_GPU_STOP_REQUESTED");
            NC01MainSnapshotV1 after = MainSnapshotCore();
            if (!after.StrictEquals(before)) { DisableCaptures("OBS_GPU_SNAPSHOT_MISMATCH"); throw new InvalidOperationException(_disabledReason); }

            RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_POST_GATE", true, "OBS_GPU_STOP_REQUESTED");
            EnsureEyeStagesMatchBaseline(0);
            EnsureEyeStagesMatchBaseline(1);
            string postFingerprint, postBuildError;
            string[] postSubset;
            if (!TryBuildRuntimeQualityFingerprint(after.Serialized, out postFingerprint, out postSubset, out postBuildError))
            { DisableCaptures(postBuildError); throw new InvalidOperationException(_disabledReason); }
            if (!string.Equals(_runtimeQualityFingerprint, postFingerprint, StringComparison.Ordinal))
            { DisableCaptures("OBS_GPU_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(postSubset)); throw new InvalidOperationException(_disabledReason); }
            VerifyRuntimeIdentityBindings();
            double[] gateLeft16; double gateLeftFov; double gateLeftAspect; bool gateLeftOrtho;
            ValidateRawEyeProjection(0, camLeft, left16, leftFovY, sourceAspect, "post-gate", out gateLeft16, out gateLeftFov, out gateLeftAspect, out gateLeftOrtho);
            double[] gateRight16; double gateRightFov; double gateRightAspect; bool gateRightOrtho;
            ValidateRawEyeProjection(1, camRight, right16, rightFovY, sourceAspect, "post-gate", out gateRight16, out gateRightFov, out gateRightAspect, out gateRightOrtho);
            for (int i = 0; i < 16; i++)
            {
                if (!RawClose(gateLeft16[i], leftRendered16[i]))
                    throw new InvalidOperationException("OBS_GPU_POST_GATE_VIEW_READBACK_MISMATCH:eye=0|index=" + i.ToString(CultureInfo.InvariantCulture)
                        + "|rendered=" + FormatInvariant(leftRendered16[i]) + "|gate=" + FormatInvariant(gateLeft16[i]));
                if (!RawClose(gateRight16[i], rightRendered16[i]))
                    throw new InvalidOperationException("OBS_GPU_POST_GATE_VIEW_READBACK_MISMATCH:eye=1|index=" + i.ToString(CultureInfo.InvariantCulture)
                        + "|rendered=" + FormatInvariant(rightRendered16[i]) + "|gate=" + FormatInvariant(gateRight16[i]));
            }

            // Atomic managed commit: the final shouldStop runs outside the gate lock first and may re-enter RequestStop
            // while returning false; the locked re-verification still refuses. A stop here keeps BOTH busy registered,
            // rejects the WHOLE pair with OperationCanceledException (non-sticky) and stamps nothing. The caller MUST
            // discard this ticket on ANY throw above (including a post-gate failure); the returned PreparedTicket is
            // only committable after this method returns AND the caller's own running/admission/epoch/generation
            // re-check still passes.
            if (shouldStop()) throw new OperationCanceledException("OBS_GPU_STOP_REQUESTED:group=OBS_GPU_COMMIT|generation=" + generation.ToString(CultureInfo.InvariantCulture));
            bool commitStopCancel;
            string commitRejectReason;
            bool committed;
            lock (_gateLock)
            {
                committed = RuntimeGroupGrant(generation, "OBS_GPU_COMMIT", true, out commitStopCancel, out commitRejectReason);
                if (committed)
                {
                    _lastBefore = before;
                    _lastAfter = after;
                }
            }
            if (!committed)
            {
                if (commitStopCancel) throw new OperationCanceledException("OBS_GPU_STOP_REQUESTED:group=OBS_GPU_COMMIT|generation=" + generation.ToString(CultureInfo.InvariantCulture) + "|recheck=1");
                throw new InvalidOperationException(commitRejectReason);
            }
            totalSw.Stop();
            return new NC01GpuPreparedPairV1
            {
                PreparedTicket = preparedTicket,
                Generation = generation,
                Width = TargetWidth,
                Height = TargetHeight,
                LeftActualView = leftRendered16,
                RightActualView = rightRendered16,
                LeftRenderRc = leftRenderRc,
                RightRenderRc = rightRenderRc,
                LeftSurfaceHex = leftScene.ToInt64().ToString("x16", CultureInfo.InvariantCulture),
                RightSurfaceHex = rightScene.ToInt64().ToString("x16", CultureInfo.InvariantCulture),
                MainBefore = before.Serialized,
                MainAfter = after.Serialized,
                MainPreserved = true,
                LeftLocked = leftLocked,
                RightLocked = rightLocked,
                LeftUnlocked = leftUnlocked,
                RightUnlocked = rightUnlocked,
                LeftRenderMs = leftRenderMs,
                RightRenderMs = rightRenderMs,
                ProbeMs = probeMs,
                TotalMs = totalSw.Elapsed.TotalMilliseconds
            };
        }
#if VRSIM_QA_D9_BEGIN_POLL
        private const uint _qaTokenStructBytes    = 32u;
        private const uint _qaTicketStructBytes   = 64u;
        private const uint _qaSourceTicketWidth   = 4096u;
        private const uint _qaSourceTicketHeight  = 4096u;
        private const uint _qaTicketFormat88      = 88u;

        private static bool SplitQaStampValid(
            NC01GpuSplitStampV1 stamp,
            int generation)
        {
            if (stamp.FrameSerial       == 0UL) return false;
            if (stamp.PoseSerial        == 0UL) return false;
            if (stamp.XrEpoch           == 0UL) return false;
            if (stamp.Generation        <= 0)   return false;
            if (stamp.Generation        != generation) return false;
            if (stamp.PoseSampleBeginQpc <= 0L) return false;
            if (stamp.PoseSampleBeginQpc >  stamp.PoseSampleEndQpc)  return false;
            if (stamp.PoseSampleEndQpc   >  stamp.CaptureBeginQpc)   return false;
            return true;
        }

        private static bool SplitQaTokenValid(
            NC01GpuSplitTokenV1 token,
            NC01GpuSplitStampV1 stamp)
        {
            if (!SplitQaStampValid(stamp, stamp.Generation)) return false;
            if (token.Version     != 1u)                     return false;
            if (token.StructBytes != _qaTokenStructBytes)    return false;
            if (token.SlotIndex   >= 3u)                    return false;
            if (token.Generation  == 0u)                    return false;
            if (token.Epoch       == 0UL)                   return false;
            if (token.Nonce       == 0UL)                   return false;
            if (token.Generation  != (uint)stamp.Generation) return false;
            if (token.Epoch       != stamp.XrEpoch)          return false;
            return true;
        }

        private static bool SplitQaTokenMatches(
            NC01GpuSplitTokenV1 actual,
            NC01GpuSplitTokenV1 original)
        {
            if (actual.Version     != original.Version)     return false;
            if (actual.StructBytes != original.StructBytes) return false;
            if (actual.SlotIndex   != original.SlotIndex)   return false;
            if (actual.Generation  != original.Generation)  return false;
            if (actual.Epoch       != original.Epoch)       return false;
            if (actual.Nonce       != original.Nonce)       return false;
            return true;
        }

        private static bool SplitQaTokenZero(NC01GpuSplitTokenV1 token)
        {
            if (token.Version     != 0u)  return false;
            if (token.StructBytes != 0u)  return false;
            if (token.SlotIndex   != 0u)  return false;
            if (token.Generation  != 0u)  return false;
            if (token.Epoch       != 0UL) return false;
            if (token.Nonce       != 0UL) return false;
            return true;
        }

        private static bool SplitQaWholeTicketZero(NC01GpuSplitNativeReplyV1 reply)
        {
            if (reply.TicketVersion      != 0u)  return false;
            if (reply.TicketStructBytes  != 0u)  return false;
            if (reply.TicketSlotIndex    != 0u)  return false;
            if (reply.TicketGeneration   != 0u)  return false;
            if (reply.TicketEpoch        != 0UL) return false;
            if (reply.PreparedTicket     != 0UL) return false;
            if (reply.LegacyLeft         != 0UL) return false;
            if (reply.LegacyRight        != 0UL) return false;
            if (reply.TicketWidth        != 0u)  return false;
            if (reply.TicketHeight       != 0u)  return false;
            if (reply.TicketFormat88     != 0u)  return false;
            if (reply.TicketReserved     != 0u)  return false;
            return true;
        }

        private static bool SplitQaTicketValid(
            NC01GpuSplitNativeReplyV1 reply,
            NC01GpuSplitStampV1 stamp)
        {
            if (!SplitQaStampValid(stamp, stamp.Generation)) return false;
            if (reply.TicketVersion     != 1u)                    return false;
            if (reply.TicketStructBytes != _qaTicketStructBytes)  return false;
            if (reply.TicketSlotIndex   >= 3u)                    return false;
            if (reply.TicketGeneration  != (uint)stamp.Generation) return false;
            if (reply.TicketEpoch       != stamp.XrEpoch)          return false;
            if (reply.PreparedTicket    == 0UL)                    return false;
            if (reply.TicketWidth       != _qaSourceTicketWidth)   return false;
            if (reply.TicketHeight      != _qaSourceTicketHeight)  return false;
            if (reply.TicketFormat88    != _qaTicketFormat88)      return false;
            if (reply.TicketReserved    != 0u)                     return false;
            return true;
        }

        private static bool SplitQaReadbackValid(
            uint op,
            NC01GpuSplitNativeReplyV1 reply)
        {
            if (op < 1u || op > 3u)                    return false;
            if (reply.ReadbackShapeValid != 1u)        return false;
            if (reply.ResultVersion      != 1u)        return false;
            if (op == 2u) { if (reply.ResultStructBytes != 56u) return false; }
            else          { if (reply.ResultStructBytes != 80u) return false; }
            if (reply.Status            >  8u)         return false;
            if (reply.LedgerStateAfter  >  2u)         return false;
            if (reply.GetDataCallsThisCall > 1u)       return false;
            if (op == 1u || op == 3u)
            {
                if ((reply.CopyExecMask & ~3u) != 0u)  return false;
                if ((reply.CopyOkMask   & ~3u) != 0u)  return false;
                if (reply.FaultConsumed > 1u)          return false;
            }
            else
            {
                if ((reply.CopyOkMask & ~3u) != 0u)    return false;
                if (reply.TicketGranted > 1u)          return false;
            }
            return true;
        }

        private static bool SplitQaBeginPending(
            uint op,
            NC01GpuSplitNativeReplyV1 reply,
            NC01GpuSplitStampV1 stamp)
        {
            if (op != 1u)                                           return false;
            if (!SplitQaReadbackValid(op, reply))                   return false;
            if (!SplitQaStampValid(stamp, stamp.Generation))         return false;
            if (reply.NativeRc        != 1)                          return false;
            if (reply.Status          != 3u)                         return false;
            if (reply.BridgeHresult   != 1)                          return false;
            if (reply.LedgerStateAfter != 1u)                        return false;
            if (!SplitQaTokenValid(reply.Token, stamp))              return false;
            if (!SplitQaWholeTicketZero(reply))                      return false;
            return true;
        }

        private static bool SplitQaPollPending(
            uint op,
            NC01GpuSplitNativeReplyV1 reply,
            NC01GpuSplitStampV1 stamp,
            NC01GpuSplitTokenV1 original)
        {
            if (op != 2u)                                           return false;
            if (!SplitQaReadbackValid(op, reply))                   return false;
            if (!SplitQaStampValid(stamp, stamp.Generation))         return false;
            if (reply.NativeRc        != 1)                          return false;
            if (reply.Status          != 3u)                         return false;
            if (reply.BridgeHresult   != 1)                          return false;
            if (reply.LedgerStateAfter != 1u)                        return false;
            if (!SplitQaTokenValid(original, stamp))                 return false;
            if (!SplitQaTokenMatches(reply.Token, original))         return false;
            if (reply.TicketGranted   != 0u)                         return false;
            if (!SplitQaWholeTicketZero(reply))                      return false;
            return true;
        }

        private static bool SplitQaActualGrant(
            uint op,
            NC01GpuSplitNativeReplyV1 reply,
            NC01GpuSplitStampV1 stamp,
            NC01GpuSplitTokenV1 original)
        {
            if (op == 1u)
            {
                if (!SplitQaReadbackValid(op, reply))               return false;
                if (!SplitQaStampValid(stamp, stamp.Generation))     return false;
                if (reply.NativeRc        != 0)                      return false;
                if (reply.Status          != 4u)                     return false;
                if (reply.BridgeHresult   != 0)                      return false;
                if (reply.LedgerStateAfter != 0u)                    return false;
                if (reply.CopyExecMask    != 3u)                     return false;
                if (reply.CopyOkMask      != 3u)                     return false;
                if (!SplitQaTokenZero(reply.Token))                  return false;
                if (!SplitQaTicketValid(reply, stamp))               return false;
                return true;
            }
            if (op == 2u)
            {
                if (!SplitQaReadbackValid(op, reply))               return false;
                if (!SplitQaStampValid(stamp, stamp.Generation))     return false;
                if (reply.NativeRc        != 0)                      return false;
                if (reply.Status          != 5u)                     return false;
                if (reply.BridgeHresult   != 0)                      return false;
                if (reply.LedgerStateAfter != 0u)                    return false;
                if (reply.CopyOkMask      != 3u)                     return false;
                if (reply.TicketGranted   != 1u)                     return false;
                if (!SplitQaTokenValid(original, stamp))             return false;
                if (!SplitQaTokenMatches(reply.Token, original))     return false;
                if (!SplitQaTicketValid(reply, stamp))               return false;
                if (reply.TicketSlotIndex  != original.SlotIndex)    return false;
                if (reply.TicketGeneration != original.Generation)   return false;
                if (reply.TicketEpoch      != original.Epoch)        return false;
                return true;
            }
            return false;
        }

        private static bool SplitQaCancelAccepted(
            uint op,
            NC01GpuSplitNativeReplyV1 reply,
            NC01GpuSplitStampV1 stamp,
            NC01GpuSplitTokenV1 original)
        {
            if (op != 3u)                                           return false;
            if (!SplitQaReadbackValid(op, reply))                   return false;
            if (!SplitQaStampValid(stamp, stamp.Generation))         return false;
            if (reply.NativeRc        != 1)                          return false;
            if (reply.LedgerStateAfter != 1u)                        return false;
            if (reply.BridgeHresult   != 0 && reply.BridgeHresult != 1) return false;
            if (reply.GetDataCallsThisCall != 0u)                    return false;
            if (!SplitQaTokenValid(original, stamp))                 return false;
            if (!SplitQaTokenMatches(reply.Token, original))         return false;
            return true;
        }

        private static bool SplitQaPollNoGrantTerminal(
            uint op,
            NC01GpuSplitNativeReplyV1 reply,
            NC01GpuSplitStampV1 stamp,
            NC01GpuSplitTokenV1 original)
        {
            if (op != 2u)                                           return false;
            if (!SplitQaReadbackValid(op, reply))                   return false;
            if (!SplitQaStampValid(stamp, stamp.Generation))         return false;
            if (reply.LedgerStateAfter != 0u)                        return false;
            if (reply.TicketGranted   != 0u)                         return false;
            if (!SplitQaWholeTicketZero(reply))                      return false;
            if (!SplitQaTokenValid(original, stamp))                 return false;
            if (!SplitQaTokenMatches(reply.Token, original))         return false;
            bool branchNoGrant      = (reply.NativeRc == -3 && reply.Status == 6u);
            bool branchFailedDrained = (reply.NativeRc == -5 && reply.Status == 7u);
            if (!branchNoGrant && !branchFailedDrained)              return false;
            return true;
        }

        private static bool SplitQaBeginRollback(
            uint op,
            NC01GpuSplitNativeReplyV1 reply)
        {
            if (op != 1u)                                           return false;
            if (!SplitQaReadbackValid(op, reply))                   return false;
            if (reply.NativeRc        != -5)                        return false;
            if (reply.Status          != 2u)                        return false;
            if (reply.LedgerStateAfter != 0u)                       return false;
            if (!SplitQaTokenZero(reply.Token))                     return false;
            if (!SplitQaWholeTicketZero(reply))                     return false;
            return true;
        }
#endif
#if VRSIM_QA_D9_BEGIN_POLL
        // QA-only private storage row: preallocated managed fields plus pure first-write reply capture and
        // B1-predicate consumption. Holds NO SDK references yet, claims NO lock, performs NO native cleanup;
        // classification flags are packet evidence only, never drain/Unlock/State transitions.
        private sealed class NC01GpuSplitRowQa
        {
            public readonly NC01HostedSceneOwner Owner;
            public readonly NC01GpuSplitStampV1 Stamp;
            public readonly int Generation;

            public Func<bool> ShouldStop;
            public Func<ulong, ulong, NC01GpuSplitNativeReplyV1> Begin;
            public Func<NC01GpuSplitTokenV1, NC01GpuSplitNativeReplyV1> Poll;
            public Func<NC01GpuSplitTokenV1, NC01GpuSplitNativeReplyV1> Cancel;

            public IWindow3D LeftEye;
            public IWindow3D RightEye;
            public VisualComponents.Create3D.IViewport LeftViewport;
            public VisualComponents.Create3D.IViewport RightViewport;
            public object LeftView;
            public object RightView;
            public ICamera LeftCamera;
            public ICamera RightCamera;
            public D3DImage LeftImage;
            public D3DImage RightImage;

            public NC01MainSnapshotV1 MainBefore;
            public NC01MainSnapshotV1 MainAfter;
            public int QualityGeneration;
            public string QualityFingerprint;
            public double LeftFov;
            public double RightFov;
            public double SourceAspect;
            public ulong LeftSurfaceIdentity;
            public ulong RightSurfaceIdentity;

            public double[] LeftExpected16;
            public double[] RightExpected16;
            public double[] LeftRendered16;
            public double[] RightRendered16;
            public NC01GpuPreparedPairV1 PreparedResult;

            public NC01GpuSplitTokenV1 OriginalToken;
            public NC01GpuSplitNativeReplyV1 FirstBeginReply;
            public NC01GpuSplitNativeReplyV1 LatestReply;
            public NC01GpuSplitNativeReplyV1 FirstGrantReply;
            public uint LatestOp;
            public uint FirstGrantOp;
            public bool HasBeginReply;
            public bool HasOriginalToken;
            public bool HasFirstGrant;
            public bool EverPending;
            public bool CancelAccepted;
            public bool DuplicateBeginReply;
            public bool ReplyAfterFirstGrant;

            public uint LockGrantedMask;
            public uint UnlockAttemptedMask;
            public uint UnlockedMask;
            public Exception BodyFailure;
            public Exception RightUnlockFailure;
            public Exception LeftUnlockFailure;
            public int LeftRenderRc;
            public int RightRenderRc;
            public double LeftRenderMs;
            public double RightRenderMs;
            public double ProbeCpuMs;
            public double InvocationCpuMs;
            // OWNER_PHASE_DECL_ROW_BEGIN
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
            public long P0Ticks;
            public int P0AttemptCount;
            public int P0ReturnCount;
            public int P0SuccessCount;
            public long P1Ticks;
            public int P1AttemptCount;
            public int P1ReturnCount;
            public int P1SuccessCount;
            public long P2Ticks;
            public int P2AttemptCount;
            public int P2ReturnCount;
            public int P2SuccessCount;
            public long P3Ticks;
            public int P3AttemptCount;
            public int P3ReturnCount;
            public int P3SuccessCount;
            public long P4Ticks;
            public int P4AttemptCount;
            public int P4ReturnCount;
            public int P4SuccessCount;
            public long P5Ticks;
            public int P5AttemptCount;
            public int P5ReturnCount;
            public int P5SuccessCount;
            public long P6Ticks;
            public int P6AttemptCount;
            public int P6ReturnCount;
            public int P6SuccessCount;
            public uint AttemptedMask;
            public uint ReturnedMask;
            public uint SuccessMask;
            public uint TimingKnownMask;
            public uint TimingInvalidMask;
            public uint CallFailedMask;
            public uint DuplicateMask;
            public uint OpenPhaseMask;
            public int OpenPhaseId;
            public long OpenStartQpc;
            public bool OwnerAcceptedRecorded;
            // MAIN_SNAPSHOT_INNER_ROW_BEGIN
            // Independent P0 and P4 inner-timing samples for the SAME row. Each phase keeps its own six timestamps plus
            // attempt/return/known/invalid markers and a bracket open-state so a P0 completion never clears P4 (and vice
            // versa); both are retained until the single-row supplement commits or rejects them together.
            public long P0InnerQcStart;
            public long P0InnerQcEnd;
            public long P0InnerFlStart;
            public long P0InnerFlEnd;
            public long P0InnerRsStart;
            public long P0InnerRsEnd;
            public int P0InnerAttempt;
            public int P0InnerReturn;
            public int P0InnerKnown;
            public int P0InnerInvalid;
            public int P0InnerOpen;
            public long P4InnerQcStart;
            public long P4InnerQcEnd;
            public long P4InnerFlStart;
            public long P4InnerFlEnd;
            public long P4InnerRsStart;
            public long P4InnerRsEnd;
            public int P4InnerAttempt;
            public int P4InnerReturn;
            public int P4InnerKnown;
            public int P4InnerInvalid;
            public int P4InnerOpen;
            // MAIN_SNAPSHOT_INNER_ROW_END
#endif
            // OWNER_PHASE_DECL_ROW_END

            public bool ReplyPending;
            public bool ReplyGrant;
            public bool ReplyCancelAccepted;
            public bool ReplyPollTerminal;
            public bool ReplyBeginRollback;
            public bool ReplyUnknown;
            public bool FaultSeen;
            public bool UnexpectedReply;

            public int State;
            public bool CallBusy;
            public bool StopLatched;
            public bool NativeEntered;
            public bool NativeTerminal;

            public NC01GpuSplitRowQa(NC01HostedSceneOwner owner, NC01GpuSplitStampV1 stamp)
            {
                Owner = owner;
                Stamp = stamp;
                Generation = stamp.Generation;
                LeftExpected16 = new double[16];
                RightExpected16 = new double[16];
                LeftRendered16 = new double[16];
                RightRendered16 = new double[16];
                PreparedResult = new NC01GpuPreparedPairV1();
            }
        }

        private NC01GpuSplitRowQa _qaSplitRow;
        // OWNER_PHASE_DECL_CACHE_BEGIN
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
        private sealed class NC01OwnerPhaseCpuWallCacheQa
        {
            public int SchemaVersion;
            public int CoverageRevision;
            public int Enabled;
            public int GateStatus;
            public int LastRowState;
            public int NativeCommitKnown;
            public long Frequency;
            public long Seq;
            public long OwnerAcceptedTotal;
            public long OwnerAcceptedAllPhasesValidCount;
            public long OwnerAcceptedInvalidPhaseCount;
            public uint ObservedGeneration;
            public uint OverflowMask;
            public uint LifetimeIdentityMismatch;
            public ulong ObservedEpoch;
            public ulong LastFrame;
            public ulong LastPose;
            public ulong FirstOwnerAcceptedFrame;
            public ulong LastOwnerAcceptedFrame;
            public ulong FirstAllValidFrame;
            public ulong LastAllValidFrame;
            public long FirstOwnerAcceptedCaptureBeginQpc;
            public long LastOwnerAcceptedCaptureBeginQpc;
            public long FirstAllValidCaptureBeginQpc;
            public long LastAllValidCaptureBeginQpc;
            public uint LastAttemptedMask;
            public uint LastReturnedMask;
            public uint LastSuccessMask;
            public uint LastTimingKnownMask;
            public uint LastTimingInvalidMask;
            public uint LastCallFailedMask;
            public uint LastDuplicateMask;
            public uint LastOpenPhaseMask;
            public long LastP0Ticks;
            public long AcceptedP0SumTicks;
            public long AcceptedP0MaxTicks;
            public int LastP0AttemptCount;
            public int LastP0ReturnCount;
            public int LastP0SuccessCount;
            public long LastP1Ticks;
            public long AcceptedP1SumTicks;
            public long AcceptedP1MaxTicks;
            public int LastP1AttemptCount;
            public int LastP1ReturnCount;
            public int LastP1SuccessCount;
            public long LastP2Ticks;
            public long AcceptedP2SumTicks;
            public long AcceptedP2MaxTicks;
            public int LastP2AttemptCount;
            public int LastP2ReturnCount;
            public int LastP2SuccessCount;
            public long LastP3Ticks;
            public long AcceptedP3SumTicks;
            public long AcceptedP3MaxTicks;
            public int LastP3AttemptCount;
            public int LastP3ReturnCount;
            public int LastP3SuccessCount;
            public long LastP4Ticks;
            public long AcceptedP4SumTicks;
            public long AcceptedP4MaxTicks;
            public int LastP4AttemptCount;
            public int LastP4ReturnCount;
            public int LastP4SuccessCount;
            public long LastP5Ticks;
            public long AcceptedP5SumTicks;
            public long AcceptedP5MaxTicks;
            public int LastP5AttemptCount;
            public int LastP5ReturnCount;
            public int LastP5SuccessCount;
            public long LastP6Ticks;
            public long AcceptedP6SumTicks;
            public long AcceptedP6MaxTicks;
            public int LastP6AttemptCount;
            public int LastP6ReturnCount;
            public int LastP6SuccessCount;
            public double AcceptedOwnerTotalSumMs;
            public double AcceptedLeftRenderSumMs;
            public double AcceptedRightRenderSumMs;
            public double AcceptedProbeSumMs;
        }
        private NC01OwnerPhaseCpuWallCacheQa _qaOwnerPhaseCpuWallCacheQa = SplitQaCreateOwnerPhaseCpuWallCacheQa();

        // OWNER_PHASE_COLD_FACTORY_BEGIN
        private static NC01OwnerPhaseCpuWallCacheQa SplitQaCreateOwnerPhaseCpuWallCacheQa()
        {
            NC01OwnerPhaseCpuWallCacheQa cache = null;
            try
            {
                cache = new NC01OwnerPhaseCpuWallCacheQa();
                cache.SchemaVersion = 1;
                cache.CoverageRevision = 1;
                cache.NativeCommitKnown = 0;
            }
            catch
            {
                return null;
            }

            string envValue = null;
            try
            {
                envValue = Environment.GetEnvironmentVariable("VRSIM_QA_OWNER_PHASE_CPU_WALL");
            }
            catch
            {
                cache.GateStatus = 3;
                cache.Enabled = 0;
                return cache;
            }

            if (string.IsNullOrEmpty(envValue))
            {
                cache.GateStatus = 0;
                cache.Enabled = 0;
            }
            else if (string.Equals(envValue, "owner-phase-cached-wall-v1", StringComparison.Ordinal))
            {
                if (Stopwatch.Frequency > 0)
                {
                    cache.GateStatus = 1;
                    cache.Enabled = 1;
                    cache.Frequency = Stopwatch.Frequency;
                }
                else
                {
                    cache.GateStatus = 4;
                    cache.Enabled = 0;
                }
            }
            else
            {
                cache.GateStatus = 2;
                cache.Enabled = 0;
            }

            return cache;
        }
        // OWNER_PHASE_COLD_FACTORY_END

        // MAIN_SNAPSHOT_INNER_DECL_BEGIN
        // Bounded, timing-only companion for the P0/P4 MainSnapshotCore inner split. It is a private primitive field
        // set (no SDK instance, dictionary, array or string) allocated once in the owner behind the existing combined
        // macro. SchemaVersion is the allocation success marker; Enabled/GateStatus/Frequency are copied from the
        // already frozen owner-phase cold gate so the environment and SDK clock are NEVER read a second time. It owns
        // no Seq field; its mutations are published only inside the existing owner Seq odd/even transaction. Any
        // initialization or accumulation failure leaves ONLY this diagnostic unavailable and never touches the
        // original 79 owner-phase fields or product state.
        #if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
        private sealed class NC01MainSnapshotInnerCpuWallCacheQa
        {
            // 15 header primitive fields.
            public int SchemaVersion;
            public int Enabled;
            public int GateStatus;
            public long Frequency;
            public uint ObservedGeneration;
            public ulong ObservedEpoch;
            public long CommonAllValidCount;
            public long InnerCompleteCount;
            public long InnerInvalidCount;
            public uint OverflowMask;
            public uint LifetimeIdentityMismatch;
            public ulong FirstCommonFrame;
            public ulong LastCommonFrame;
            public long FirstCommonCaptureBeginQpc;
            public long LastCommonCaptureBeginQpc;
            // 8 SumTicks (long) + 8 SampleCount (long): P0/P4 x ConfiguredQuality/ActualFlags/RenderStages/ScalarFormattingResidual.
            public long P0ConfiguredQualitySumTicks;
            public long P0ActualFlagsSumTicks;
            public long P0RenderStagesSumTicks;
            public long P0ScalarFormattingResidualSumTicks;
            public long P4ConfiguredQualitySumTicks;
            public long P4ActualFlagsSumTicks;
            public long P4RenderStagesSumTicks;
            public long P4ScalarFormattingResidualSumTicks;
            public long P0ConfiguredQualitySampleCount;
            public long P0ActualFlagsSampleCount;
            public long P0RenderStagesSampleCount;
            public long P0ScalarFormattingResidualSampleCount;
            public long P4ConfiguredQualitySampleCount;
            public long P4ActualFlagsSampleCount;
            public long P4RenderStagesSampleCount;
            public long P4ScalarFormattingResidualSampleCount;
        }

        private NC01MainSnapshotInnerCpuWallCacheQa _qaMainSnapshotInnerCpuWallCacheQa;
        // One-shot cold-allocation gate: the companion is attempted at most once for the owner's lifetime. If the
        // single attempt fails the companion stays null (unavailable) and is NEVER retried on a later accepted frame.
        private bool _qaMainSnapshotInnerAllocAttempted;

        // Allocates the companion exactly once, deriving Enabled/GateStatus/Frequency from the already frozen
        // owner-phase cold gate; the environment and SDK clock are never read again. Called once from the owner's
        // cold constructor path; the accepted-frame supplement only reads the existing companion or null.
        // Failure is nonintrusive: it leaves the companion null (unavailable) without changing product, acceptance,
        // Stop or owner-phase state.
        private static NC01MainSnapshotInnerCpuWallCacheQa SplitQaMainSnapshotInnerEnsureQa(
            NC01HostedSceneOwner owner, NC01OwnerPhaseCpuWallCacheQa ownerPhase)
        {
            if (owner == null) return null;
            NC01MainSnapshotInnerCpuWallCacheQa inner = owner._qaMainSnapshotInnerCpuWallCacheQa;
            if (inner != null) return inner;
            // Lifetime one-shot: once an allocation has been attempted (success or failure) never try again, so a failed
            // cold allocation keeps the companion unavailable without a per-frame new() on the accepted hot path.
            if (owner._qaMainSnapshotInnerAllocAttempted) return null;
            owner._qaMainSnapshotInnerAllocAttempted = true;
            try
            {
                inner = new NC01MainSnapshotInnerCpuWallCacheQa();
                if (ownerPhase != null && ownerPhase.SchemaVersion == 1)
                {
                    inner.Enabled = ownerPhase.Enabled;
                    inner.GateStatus = ownerPhase.GateStatus;
                    inner.Frequency = ownerPhase.Frequency;
                }
                inner.SchemaVersion = 1;
                owner._qaMainSnapshotInnerCpuWallCacheQa = inner;
                return inner;
            }
            catch
            {
                owner._qaMainSnapshotInnerCpuWallCacheQa = null;
                return null;
            }
        }
        // MAIN_SNAPSHOT_INNER_DECL_END
#endif
        // OWNER_PHASE_HOT_TIMING_READY_BEGIN
        private static NC01OwnerPhaseCpuWallCacheQa SplitQaOwnerPhaseReadGateQa(NC01GpuSplitRowQa row)
        {
            try
            {
                if (row == null || row.Owner == null)
                    return null;

                var cache = row.Owner._qaOwnerPhaseCpuWallCacheQa;
                if (cache == null)
                    return null;
                if (cache.Enabled != 1)
                    return null;
                if (cache.Frequency <= 0L)
                    return null;
                if (cache.LifetimeIdentityMismatch != 0U)
                    return null;

                if (row.Generation <= 0)
                    return null;
                if (row.Generation != row.Stamp.Generation)
                    return null;
                if (row.Stamp.FrameSerial == 0UL)
                    return null;
                if (row.Stamp.PoseSerial == 0UL)
                    return null;
                if (row.Stamp.XrEpoch == 0UL)
                    return null;
                if (row.Stamp.CaptureBeginQpc <= 0L)
                    return null;

                uint observedGen = cache.ObservedGeneration;
                ulong observedEpoch = cache.ObservedEpoch;

                if (observedGen == 0U && observedEpoch == 0UL)
                    return cache;

                if (observedGen == 0U || observedEpoch == 0UL)
                    return null;

                if (observedGen != (uint)row.Generation)
                    return null;
                if (observedEpoch != row.Stamp.XrEpoch)
                    return null;

                return cache;
            }
            catch
            {
                return null;
            }
        }
        // OWNER_PHASE_HOT_TIMING_READY_END
        // OWNER_PHASE_HOT_TIMING_CORE_BEGIN
        private static void SplitQaOwnerPhaseStartQa(NC01GpuSplitRowQa row, int phase)
        {
            try
            {
                NC01OwnerPhaseCpuWallCacheQa cache = SplitQaOwnerPhaseReadGateQa(row);
                if (cache == null)
                    return;

                if (phase < 0 || phase > 6)
                {
                    if (row.OpenPhaseMask != 0U)
                    {
                        uint oldBit = row.OpenPhaseMask;
                        row.TimingInvalidMask |= oldBit;
                        row.TimingKnownMask &= ~oldBit;
                        row.OpenPhaseMask = 0U;
                        row.OpenStartQpc = 0L;
                        row.OpenPhaseId = 0;
                    }
                    return;
                }

                uint phaseBit = 1U << phase;

                bool wasAlreadyAttempted = (row.AttemptedMask & phaseBit) != 0U;

                bool attemptSaturated = false;
                switch (phase)
                {
                    case 0: if (row.P0AttemptCount < int.MaxValue) row.P0AttemptCount++; else attemptSaturated = true; break;
                    case 1: if (row.P1AttemptCount < int.MaxValue) row.P1AttemptCount++; else attemptSaturated = true; break;
                    case 2: if (row.P2AttemptCount < int.MaxValue) row.P2AttemptCount++; else attemptSaturated = true; break;
                    case 3: if (row.P3AttemptCount < int.MaxValue) row.P3AttemptCount++; else attemptSaturated = true; break;
                    case 4: if (row.P4AttemptCount < int.MaxValue) row.P4AttemptCount++; else attemptSaturated = true; break;
                    case 5: if (row.P5AttemptCount < int.MaxValue) row.P5AttemptCount++; else attemptSaturated = true; break;
                    case 6: if (row.P6AttemptCount < int.MaxValue) row.P6AttemptCount++; else attemptSaturated = true; break;
                }

                row.AttemptedMask |= phaseBit;

                if (attemptSaturated)
                {
                    row.TimingInvalidMask |= phaseBit;
                    row.TimingKnownMask &= ~phaseBit;
                }

                if (wasAlreadyAttempted)
                {
                    row.DuplicateMask |= phaseBit;
                    row.TimingInvalidMask |= phaseBit;
                    row.TimingKnownMask &= ~phaseBit;
                }

                if (row.OpenPhaseMask != 0U)
                {
                    uint oldBit = row.OpenPhaseMask;
                    row.TimingInvalidMask |= oldBit;
                    row.TimingInvalidMask |= phaseBit;
                    row.TimingKnownMask &= ~oldBit;
                    row.TimingKnownMask &= ~phaseBit;
                    row.OpenPhaseMask = 0U;
                    row.OpenStartQpc = 0L;
                    row.OpenPhaseId = 0;
                    return;
                }

                if (wasAlreadyAttempted || attemptSaturated)
                    return;

                row.OpenPhaseId = phase;
                row.OpenPhaseMask = phaseBit;

                long ts;
                try
                {
                    ts = Stopwatch.GetTimestamp();
                }
                catch
                {
                    ts = 0L;
                }

                if (ts <= 0L)
                {
                    row.TimingInvalidMask |= phaseBit;
                    row.OpenStartQpc = 0L;
                }
                else
                {
                    row.OpenStartQpc = ts;
                }
            }
            catch
            {
                try
                {
                    if (row.OpenPhaseMask != 0U)
                    {
                        uint bit = row.OpenPhaseMask;
                        row.TimingInvalidMask |= bit;
                        row.TimingKnownMask &= ~bit;
                        row.OpenPhaseMask = 0U;
                        row.OpenStartQpc = 0L;
                        row.OpenPhaseId = 0;
                    }
                }
                catch
                {
                }
            }
        }

        private static void SplitQaOwnerPhaseCloseQa(NC01GpuSplitRowQa row, int phase, bool returnedNormally, bool success)
        {
            try
            {
                NC01OwnerPhaseCpuWallCacheQa cache = SplitQaOwnerPhaseReadGateQa(row);
                if (cache == null)
                    return;

                if (phase < 0 || phase > 6)
                {
                    if (row.OpenPhaseMask != 0U)
                    {
                        uint oldBit = row.OpenPhaseMask;
                        row.TimingInvalidMask |= oldBit;
                        row.TimingKnownMask &= ~oldBit;
                        row.OpenPhaseMask = 0U;
                        row.OpenStartQpc = 0L;
                        row.OpenPhaseId = 0;
                    }
                    return;
                }

                uint phaseBit = 1U << phase;

                if ((row.OpenPhaseMask & phaseBit) == 0U)
                {
                    row.TimingInvalidMask |= phaseBit;
                    row.TimingKnownMask &= ~phaseBit;
                    if (row.OpenPhaseMask != 0U)
                    {
                        uint oldBit = row.OpenPhaseMask;
                        row.TimingInvalidMask |= oldBit;
                        row.TimingKnownMask &= ~oldBit;
                        row.OpenPhaseMask = 0U;
                        row.OpenStartQpc = 0L;
                        row.OpenPhaseId = 0;
                    }
                    return;
                }

                bool alreadyInvalid = (row.TimingInvalidMask & phaseBit) != 0U;
                bool alreadyDuplicate = (row.DuplicateMask & phaseBit) != 0U;

                long now;
                try
                {
                    now = Stopwatch.GetTimestamp();
                }
                catch
                {
                    now = 0L;
                }

                long start = row.OpenStartQpc;

                bool timingValid = !alreadyInvalid && !alreadyDuplicate && start > 0L && now >= start;
                long delta = timingValid ? (now - start) : 0L;

                if (returnedNormally)
                {
                    row.ReturnedMask |= phaseBit;
                    bool retSaturated = false;
                    switch (phase)
                    {
                        case 0: if (row.P0ReturnCount < int.MaxValue) row.P0ReturnCount++; else retSaturated = true; break;
                        case 1: if (row.P1ReturnCount < int.MaxValue) row.P1ReturnCount++; else retSaturated = true; break;
                        case 2: if (row.P2ReturnCount < int.MaxValue) row.P2ReturnCount++; else retSaturated = true; break;
                        case 3: if (row.P3ReturnCount < int.MaxValue) row.P3ReturnCount++; else retSaturated = true; break;
                        case 4: if (row.P4ReturnCount < int.MaxValue) row.P4ReturnCount++; else retSaturated = true; break;
                        case 5: if (row.P5ReturnCount < int.MaxValue) row.P5ReturnCount++; else retSaturated = true; break;
                        case 6: if (row.P6ReturnCount < int.MaxValue) row.P6ReturnCount++; else retSaturated = true; break;
                    }
                    if (retSaturated)
                    {
                        row.TimingInvalidMask |= phaseBit;
                        timingValid = false;
                    }

                    if (success)
                    {
                        row.SuccessMask |= phaseBit;
                        bool succSaturated = false;
                        switch (phase)
                        {
                            case 0: if (row.P0SuccessCount < int.MaxValue) row.P0SuccessCount++; else succSaturated = true; break;
                            case 1: if (row.P1SuccessCount < int.MaxValue) row.P1SuccessCount++; else succSaturated = true; break;
                            case 2: if (row.P2SuccessCount < int.MaxValue) row.P2SuccessCount++; else succSaturated = true; break;
                            case 3: if (row.P3SuccessCount < int.MaxValue) row.P3SuccessCount++; else succSaturated = true; break;
                            case 4: if (row.P4SuccessCount < int.MaxValue) row.P4SuccessCount++; else succSaturated = true; break;
                            case 5: if (row.P5SuccessCount < int.MaxValue) row.P5SuccessCount++; else succSaturated = true; break;
                            case 6: if (row.P6SuccessCount < int.MaxValue) row.P6SuccessCount++; else succSaturated = true; break;
                        }
                        if (succSaturated)
                        {
                            row.TimingInvalidMask |= phaseBit;
                            timingValid = false;
                        }
                    }
                    else
                    {
                        row.CallFailedMask |= phaseBit;
                    }
                }
                else
                {
                    row.CallFailedMask |= phaseBit;
                }

                if (timingValid)
                {
                    switch (phase)
                    {
                        case 0: row.P0Ticks = delta; break;
                        case 1: row.P1Ticks = delta; break;
                        case 2: row.P2Ticks = delta; break;
                        case 3: row.P3Ticks = delta; break;
                        case 4: row.P4Ticks = delta; break;
                        case 5: row.P5Ticks = delta; break;
                        case 6: row.P6Ticks = delta; break;
                    }
                    row.TimingKnownMask |= phaseBit;
                }
                else
                {
                    row.TimingKnownMask &= ~phaseBit;
                }

                row.OpenPhaseMask = 0U;
                row.OpenStartQpc = 0L;
                row.OpenPhaseId = 0;
            }
            catch
            {
                try
                {
                    if (row.OpenPhaseMask != 0U)
                    {
                        uint bit = row.OpenPhaseMask;
                        row.TimingInvalidMask |= bit;
                        row.TimingKnownMask &= ~bit;
                        row.OpenPhaseMask = 0U;
                        row.OpenStartQpc = 0L;
                        row.OpenPhaseId = 0;
                    }
                }
                catch
                {
                }
            }
        }

        private static void SplitQaOwnerPhaseEndQa(NC01GpuSplitRowQa row, int phase, bool success)
        {
            SplitQaOwnerPhaseCloseQa(row, phase, true, success);
        }

        private static void SplitQaOwnerPhaseAbortQa(NC01GpuSplitRowQa row)
        {
            try
            {
                NC01OwnerPhaseCpuWallCacheQa cache = SplitQaOwnerPhaseReadGateQa(row);
                if (cache == null)
                    return;

                if (row.OpenPhaseMask == 0U)
                    return;

                int openPhase = row.OpenPhaseId;
                SplitQaOwnerPhaseCloseQa(row, openPhase, false, false);
            }
            catch
            {
            }
        }
        // OWNER_PHASE_HOT_TIMING_CORE_END

        // OWNER_PHASE_PUBLISH_HELPERS_BEGIN
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL

        private static bool QaBeginPublication(NC01OwnerPhaseCpuWallCacheQa c)
        {
            long s = Volatile.Read(ref c.Seq);
            if ((s & 1L) != 0 || s > long.MaxValue - 2)
            {
                Volatile.Write(ref c.OverflowMask, c.OverflowMask | 0x80000000u);
                return false;
            }
            Volatile.Write(ref c.Seq, s + 1);
            return true;
        }

        private static void QaEndPublication(NC01OwnerPhaseCpuWallCacheQa c)
        {
            Volatile.Write(ref c.Seq, Volatile.Read(ref c.Seq) + 1);
        }

        private static bool QaCacheActive(NC01OwnerPhaseCpuWallCacheQa c)
        {
            if (c == null) return false;
            if (c.Enabled != 1) return false;
            if (c.GateStatus != 1) return false;
            if (c.Frequency <= 0) return false;
            if (c.SchemaVersion != 1) return false;
            if (c.OverflowMask != 0) return false;
            if (c.LifetimeIdentityMismatch != 0) return false;
            return true;
        }

        private static bool QaRowValid(NC01GpuSplitRowQa row)
        {
            if (row.Owner == null) return false;
            if (row.Generation <= 0) return false;
            if (row.Generation != row.Stamp.Generation) return false;
            if (row.Stamp.FrameSerial == 0UL) return false;
            if (row.Stamp.PoseSerial == 0UL) return false;
            if (row.Stamp.XrEpoch == 0UL) return false;
            if (row.Stamp.CaptureBeginQpc <= 0L) return false;
            return true;
        }

        private static bool QaBindLifetime(NC01OwnerPhaseCpuWallCacheQa c, int generation, ulong xrEpoch)
        {
            if (c.LifetimeIdentityMismatch != 0u) return false;
            if (generation <= 0 || xrEpoch == 0UL)
            {
                c.LifetimeIdentityMismatch = 1u;
                return false;
            }
            uint gen = (uint)generation;
            if (c.ObservedGeneration == 0u && c.ObservedEpoch == 0UL)
            {
                c.ObservedGeneration = gen;
                c.ObservedEpoch = xrEpoch;
                return true;
            }
            if (c.ObservedGeneration == gen && c.ObservedEpoch == xrEpoch) return true;
            c.LifetimeIdentityMismatch = 1u;
            return false;
        }

        private static void QaPublishRowScalars(
            NC01OwnerPhaseCpuWallCacheQa c,
            NC01GpuSplitRowQa row)
        {
            c.LastFrame = row.Stamp.FrameSerial;
            c.LastPose = row.Stamp.PoseSerial;
            c.LastRowState = row.State;
            c.NativeCommitKnown = 0;
            c.LastAttemptedMask = row.AttemptedMask;
            c.LastReturnedMask = row.ReturnedMask;
            c.LastSuccessMask = row.SuccessMask;
            c.LastTimingKnownMask = row.TimingKnownMask;
            c.LastTimingInvalidMask = row.TimingInvalidMask;
            c.LastCallFailedMask = row.CallFailedMask;
            c.LastDuplicateMask = row.DuplicateMask;
            c.LastOpenPhaseMask = row.OpenPhaseMask;
            c.LastP0Ticks = row.P0Ticks;
            c.LastP0AttemptCount = row.P0AttemptCount;
            c.LastP0ReturnCount = row.P0ReturnCount;
            c.LastP0SuccessCount = row.P0SuccessCount;
            c.LastP1Ticks = row.P1Ticks;
            c.LastP1AttemptCount = row.P1AttemptCount;
            c.LastP1ReturnCount = row.P1ReturnCount;
            c.LastP1SuccessCount = row.P1SuccessCount;
            c.LastP2Ticks = row.P2Ticks;
            c.LastP2AttemptCount = row.P2AttemptCount;
            c.LastP2ReturnCount = row.P2ReturnCount;
            c.LastP2SuccessCount = row.P2SuccessCount;
            c.LastP3Ticks = row.P3Ticks;
            c.LastP3AttemptCount = row.P3AttemptCount;
            c.LastP3ReturnCount = row.P3ReturnCount;
            c.LastP3SuccessCount = row.P3SuccessCount;
            c.LastP4Ticks = row.P4Ticks;
            c.LastP4AttemptCount = row.P4AttemptCount;
            c.LastP4ReturnCount = row.P4ReturnCount;
            c.LastP4SuccessCount = row.P4SuccessCount;
            c.LastP5Ticks = row.P5Ticks;
            c.LastP5AttemptCount = row.P5AttemptCount;
            c.LastP5ReturnCount = row.P5ReturnCount;
            c.LastP5SuccessCount = row.P5SuccessCount;
            c.LastP6Ticks = row.P6Ticks;
            c.LastP6AttemptCount = row.P6AttemptCount;
            c.LastP6ReturnCount = row.P6ReturnCount;
            c.LastP6SuccessCount = row.P6SuccessCount;
        }

        private static bool QaAllValidCheck(NC01GpuSplitRowQa row, NC01GpuPreparedPairV1 dto)
        {
            const uint low7 = 0x7Fu;
            if (row.AttemptedMask != low7) return false;
            if (row.ReturnedMask != low7) return false;
            if (row.SuccessMask != low7) return false;
            if (row.TimingKnownMask != low7) return false;
            if (row.AttemptedMask != (row.AttemptedMask & low7)) return false;
            if (row.ReturnedMask != (row.ReturnedMask & low7)) return false;
            if (row.SuccessMask != (row.SuccessMask & low7)) return false;
            if (row.TimingKnownMask != (row.TimingKnownMask & low7)) return false;
            if (row.TimingInvalidMask != 0u) return false;
            if (row.CallFailedMask != 0u) return false;
            if (row.DuplicateMask != 0u) return false;
            if (row.OpenPhaseMask != 0u) return false;
            if (row.P0AttemptCount != 1 || row.P0ReturnCount != 1 || row.P0SuccessCount != 1) return false;
            if (row.P1AttemptCount != 1 || row.P1ReturnCount != 1 || row.P1SuccessCount != 1) return false;
            if (row.P2AttemptCount != 1 || row.P2ReturnCount != 1 || row.P2SuccessCount != 1) return false;
            if (row.P3AttemptCount != 1 || row.P3ReturnCount != 1 || row.P3SuccessCount != 1) return false;
            if (row.P4AttemptCount != 1 || row.P4ReturnCount != 1 || row.P4SuccessCount != 1) return false;
            if (row.P5AttemptCount != 1 || row.P5ReturnCount != 1 || row.P5SuccessCount != 1) return false;
            if (row.P6AttemptCount != 1 || row.P6ReturnCount != 1 || row.P6SuccessCount != 1) return false;
            if (row.P0Ticks < 0L) return false;
            if (row.P1Ticks < 0L) return false;
            if (row.P2Ticks < 0L) return false;
            if (row.P3Ticks < 0L) return false;
            if (row.P4Ticks < 0L) return false;
            if (row.P5Ticks < 0L) return false;
            if (row.P6Ticks < 0L) return false;
            if (dto.TotalMs < 0.0 || double.IsInfinity(dto.TotalMs) || double.IsNaN(dto.TotalMs)) return false;
            if (dto.LeftRenderMs < 0.0 || double.IsInfinity(dto.LeftRenderMs) || double.IsNaN(dto.LeftRenderMs)) return false;
            if (dto.RightRenderMs < 0.0 || double.IsInfinity(dto.RightRenderMs) || double.IsNaN(dto.RightRenderMs)) return false;
            if (dto.ProbeMs < 0.0 || double.IsInfinity(dto.ProbeMs) || double.IsNaN(dto.ProbeMs)) return false;
            return true;
        }

        private static bool QaTryRecordAcceptedCohort(
            NC01OwnerPhaseCpuWallCacheQa c,
            NC01GpuSplitRowQa row,
            NC01GpuPreparedPairV1 dto)
        {
            bool allValid = QaAllValidCheck(row, dto);

            long total = c.OwnerAcceptedTotal;
            if (total < 0L) { c.OverflowMask |= 0x00000080u; return false; }
            if (total > long.MaxValue - 1L) { c.OverflowMask |= 0x00000080u; return false; }
            long allValidCount = c.OwnerAcceptedAllPhasesValidCount;
            long invalidCount = c.OwnerAcceptedInvalidPhaseCount;
            if (allValidCount < 0L) { c.OverflowMask |= 0x00000080u; return false; }
            if (invalidCount < 0L) { c.OverflowMask |= 0x00000080u; return false; }
            if (allValidCount > total) { c.OverflowMask |= 0x00000080u; return false; }
            if (invalidCount > total) { c.OverflowMask |= 0x00000080u; return false; }
            if (total - invalidCount != allValidCount) { c.OverflowMask |= 0x00000080u; return false; }
            long bucket = allValid ? c.OwnerAcceptedAllPhasesValidCount : c.OwnerAcceptedInvalidPhaseCount;
            if (bucket < 0L) { c.OverflowMask |= 0x00000080u; return false; }
            if (bucket > long.MaxValue - 1L) { c.OverflowMask |= 0x00000080u; return false; }

            uint tickOverflow = 0u;
            long newP0Sum = 0L, newP1Sum = 0L, newP2Sum = 0L, newP3Sum = 0L, newP4Sum = 0L, newP5Sum = 0L, newP6Sum = 0L;
            if (allValid)
            {
                if (c.AcceptedP0SumTicks < 0L || row.P0Ticks < 0L || c.AcceptedP0SumTicks > long.MaxValue - row.P0Ticks)
                    tickOverflow |= 0x01u;
                else newP0Sum = c.AcceptedP0SumTicks + row.P0Ticks;

                if (c.AcceptedP1SumTicks < 0L || row.P1Ticks < 0L || c.AcceptedP1SumTicks > long.MaxValue - row.P1Ticks)
                    tickOverflow |= 0x02u;
                else newP1Sum = c.AcceptedP1SumTicks + row.P1Ticks;

                if (c.AcceptedP2SumTicks < 0L || row.P2Ticks < 0L || c.AcceptedP2SumTicks > long.MaxValue - row.P2Ticks)
                    tickOverflow |= 0x04u;
                else newP2Sum = c.AcceptedP2SumTicks + row.P2Ticks;

                if (c.AcceptedP3SumTicks < 0L || row.P3Ticks < 0L || c.AcceptedP3SumTicks > long.MaxValue - row.P3Ticks)
                    tickOverflow |= 0x08u;
                else newP3Sum = c.AcceptedP3SumTicks + row.P3Ticks;

                if (c.AcceptedP4SumTicks < 0L || row.P4Ticks < 0L || c.AcceptedP4SumTicks > long.MaxValue - row.P4Ticks)
                    tickOverflow |= 0x10u;
                else newP4Sum = c.AcceptedP4SumTicks + row.P4Ticks;

                if (c.AcceptedP5SumTicks < 0L || row.P5Ticks < 0L || c.AcceptedP5SumTicks > long.MaxValue - row.P5Ticks)
                    tickOverflow |= 0x20u;
                else newP5Sum = c.AcceptedP5SumTicks + row.P5Ticks;

                if (c.AcceptedP6SumTicks < 0L || row.P6Ticks < 0L || c.AcceptedP6SumTicks > long.MaxValue - row.P6Ticks)
                    tickOverflow |= 0x40u;
                else newP6Sum = c.AcceptedP6SumTicks + row.P6Ticks;

                if (c.AcceptedP0MaxTicks < 0L) tickOverflow |= 0x00000001u;
                if (c.AcceptedP1MaxTicks < 0L) tickOverflow |= 0x00000002u;
                if (c.AcceptedP2MaxTicks < 0L) tickOverflow |= 0x00000004u;
                if (c.AcceptedP3MaxTicks < 0L) tickOverflow |= 0x00000008u;
                if (c.AcceptedP4MaxTicks < 0L) tickOverflow |= 0x00000010u;
                if (c.AcceptedP5MaxTicks < 0L) tickOverflow |= 0x00000020u;
                if (c.AcceptedP6MaxTicks < 0L) tickOverflow |= 0x00000040u;
                if (tickOverflow != 0u)
                {
                    c.OverflowMask |= tickOverflow;
                    return false;
                }
            }

            double newOwnerTotalSum = 0.0, newLeftSum = 0.0, newRightSum = 0.0, newProbeSum = 0.0;
            if (allValid)
            {
                bool dblOk = true;
                double r;

                if (c.AcceptedOwnerTotalSumMs < 0.0 || double.IsNaN(c.AcceptedOwnerTotalSumMs) || double.IsInfinity(c.AcceptedOwnerTotalSumMs))
                    dblOk = false;
                else { r = c.AcceptedOwnerTotalSumMs + dto.TotalMs; if (double.IsNaN(r) || double.IsInfinity(r) || r < 0.0) dblOk = false; else newOwnerTotalSum = r; }

                if (dblOk)
                {
                    if (c.AcceptedLeftRenderSumMs < 0.0 || double.IsNaN(c.AcceptedLeftRenderSumMs) || double.IsInfinity(c.AcceptedLeftRenderSumMs))
                        dblOk = false;
                    else { r = c.AcceptedLeftRenderSumMs + dto.LeftRenderMs; if (double.IsNaN(r) || double.IsInfinity(r) || r < 0.0) dblOk = false; else newLeftSum = r; }
                }

                if (dblOk)
                {
                    if (c.AcceptedRightRenderSumMs < 0.0 || double.IsNaN(c.AcceptedRightRenderSumMs) || double.IsInfinity(c.AcceptedRightRenderSumMs))
                        dblOk = false;
                    else { r = c.AcceptedRightRenderSumMs + dto.RightRenderMs; if (double.IsNaN(r) || double.IsInfinity(r) || r < 0.0) dblOk = false; else newRightSum = r; }
                }

                if (dblOk)
                {
                    if (c.AcceptedProbeSumMs < 0.0 || double.IsNaN(c.AcceptedProbeSumMs) || double.IsInfinity(c.AcceptedProbeSumMs))
                        dblOk = false;
                    else { r = c.AcceptedProbeSumMs + dto.ProbeMs; if (double.IsNaN(r) || double.IsInfinity(r) || r < 0.0) dblOk = false; else newProbeSum = r; }
                }

                if (!dblOk)
                {
                    c.OverflowMask |= 0x00000100u;
                    return false;
                }
            }

            c.OwnerAcceptedTotal = total + 1L;
            if (allValid)
                c.OwnerAcceptedAllPhasesValidCount = bucket + 1L;
            else
                c.OwnerAcceptedInvalidPhaseCount = bucket + 1L;

            if (total == 0L)
            {
                c.FirstOwnerAcceptedFrame = row.Stamp.FrameSerial;
                c.FirstOwnerAcceptedCaptureBeginQpc = row.Stamp.CaptureBeginQpc;
            }
            c.LastOwnerAcceptedFrame = row.Stamp.FrameSerial;
            c.LastOwnerAcceptedCaptureBeginQpc = row.Stamp.CaptureBeginQpc;

            if (allValid)
            {
                if (c.FirstAllValidFrame == 0UL)
                {
                    c.FirstAllValidFrame = row.Stamp.FrameSerial;
                    c.FirstAllValidCaptureBeginQpc = row.Stamp.CaptureBeginQpc;
                }
                c.LastAllValidFrame = row.Stamp.FrameSerial;
                c.LastAllValidCaptureBeginQpc = row.Stamp.CaptureBeginQpc;

                c.AcceptedP0SumTicks = newP0Sum;
                c.AcceptedP1SumTicks = newP1Sum;
                c.AcceptedP2SumTicks = newP2Sum;
                c.AcceptedP3SumTicks = newP3Sum;
                c.AcceptedP4SumTicks = newP4Sum;
                c.AcceptedP5SumTicks = newP5Sum;
                c.AcceptedP6SumTicks = newP6Sum;

                if (row.P0Ticks > c.AcceptedP0MaxTicks) c.AcceptedP0MaxTicks = row.P0Ticks;
                if (row.P1Ticks > c.AcceptedP1MaxTicks) c.AcceptedP1MaxTicks = row.P1Ticks;
                if (row.P2Ticks > c.AcceptedP2MaxTicks) c.AcceptedP2MaxTicks = row.P2Ticks;
                if (row.P3Ticks > c.AcceptedP3MaxTicks) c.AcceptedP3MaxTicks = row.P3Ticks;
                if (row.P4Ticks > c.AcceptedP4MaxTicks) c.AcceptedP4MaxTicks = row.P4Ticks;
                if (row.P5Ticks > c.AcceptedP5MaxTicks) c.AcceptedP5MaxTicks = row.P5Ticks;
                if (row.P6Ticks > c.AcceptedP6MaxTicks) c.AcceptedP6MaxTicks = row.P6Ticks;

                c.AcceptedOwnerTotalSumMs = newOwnerTotalSum;
                c.AcceptedLeftRenderSumMs = newLeftSum;
                c.AcceptedRightRenderSumMs = newRightSum;
                c.AcceptedProbeSumMs = newProbeSum;
            }

            if (allValid)
                SplitQaSupplementMainSnapshotInnerQa(c, row);

            return true;
        }

        // Supplement the P0/P4 inner split only after the original all-valid accepted accumulation has succeeded,
        // inside the caller's existing owner Seq transaction. Every all-valid accepted row advances CommonAllValidCount
        // and the Common first/last frame/QPC range (identity + nonnegative + safe-add checked); the row is recorded
        // Complete ONLY when BOTH the P0 and P4 inner samples arrived exactly once, are monotonic inside their phase
        // boundaries, and yield a nonnegative scalar+formatting+overhead residual; otherwise it is recorded Invalid.
        // Common = Complete + Invalid always holds (verified by safe difference, never unchecked addition). All eight
        // sums and eight counts and every header counter are preflighted atomically into locals and committed in a
        // single pass; any overflow, identity mismatch, or exception becomes a sticky companion failure and mutates
        // NOTHING. The whole body is catch-contained so a companion failure can never escape into the outer accepted
        // record path or set the original 79 OverflowMask / product state / acceptance / record-once flag.
        private static void SplitQaSupplementMainSnapshotInnerQa(
            NC01OwnerPhaseCpuWallCacheQa c,
            NC01GpuSplitRowQa row)
        {
            NC01MainSnapshotInnerCpuWallCacheQa i = null;
            try
            {
                if (row == null || row.Owner == null) return;
                i = row.Owner._qaMainSnapshotInnerCpuWallCacheQa;
                if (i == null) return;
                if (i.SchemaVersion != 1) { i.OverflowMask |= 0x80000000u; return; }
                if (i.Enabled != 1 || i.GateStatus != 1 || i.Frequency <= 0L) return;
                if (i.OverflowMask != 0u || i.LifetimeIdentityMismatch != 0u) return;

                bool p0Ok = row.P0InnerAttempt == 1 && row.P0InnerReturn == 1 && row.P0InnerKnown == 1 && row.P0InnerInvalid == 0
                    && row.P0InnerQcStart > 0L && row.P0InnerQcEnd >= row.P0InnerQcStart
                    && row.P0InnerFlStart > 0L && row.P0InnerFlEnd >= row.P0InnerFlStart
                    && row.P0InnerRsStart > 0L && row.P0InnerRsEnd >= row.P0InnerRsStart
                    && row.P0InnerQcEnd <= row.P0InnerFlStart && row.P0InnerFlEnd <= row.P0InnerRsStart;
                bool p4Ok = row.P4InnerAttempt == 1 && row.P4InnerReturn == 1 && row.P4InnerKnown == 1 && row.P4InnerInvalid == 0
                    && row.P4InnerQcStart > 0L && row.P4InnerQcEnd >= row.P4InnerQcStart
                    && row.P4InnerFlStart > 0L && row.P4InnerFlEnd >= row.P4InnerFlStart
                    && row.P4InnerRsStart > 0L && row.P4InnerRsEnd >= row.P4InnerRsStart
                    && row.P4InnerQcEnd <= row.P4InnerFlStart && row.P4InnerFlEnd <= row.P4InnerRsStart;

                bool complete = false;
                long p0Qc = 0L, p0Fl = 0L, p0Rs = 0L, p0Res = 0L;
                long p4Qc = 0L, p4Fl = 0L, p4Rs = 0L, p4Res = 0L;
                if (p0Ok && p4Ok && row.P0Ticks >= 0L && row.P4Ticks >= 0L)
                {
                    long s0 = p0Qc = row.P0InnerQcEnd - row.P0InnerQcStart;
                    long f0 = p0Fl = row.P0InnerFlEnd - row.P0InnerFlStart;
                    long r0 = p0Rs = row.P0InnerRsEnd - row.P0InnerRsStart;
                    if (s0 >= 0L && f0 >= 0L && r0 >= 0L && s0 <= long.MaxValue - f0 && s0 + f0 <= long.MaxValue - r0
                        && row.P0Ticks >= s0 + f0 + r0)
                    {
                        p0Res = row.P0Ticks - (s0 + f0 + r0);
                        long s4 = p4Qc = row.P4InnerQcEnd - row.P4InnerQcStart;
                        long f4 = p4Fl = row.P4InnerFlEnd - row.P4InnerFlStart;
                        long r4 = p4Rs = row.P4InnerRsEnd - row.P4InnerRsStart;
                        if (s4 >= 0L && f4 >= 0L && r4 >= 0L && s4 <= long.MaxValue - f4 && s4 + f4 <= long.MaxValue - r4
                            && row.P4Ticks >= s4 + f4 + r4)
                        {
                            p4Res = row.P4Ticks - (s4 + f4 + r4);
                            complete = true;
                        }
                    }
                }

                uint gen = row.Generation <= 0 ? 0u : unchecked((uint)row.Generation);
                ulong epoch = row.Stamp.XrEpoch;
                ulong frame = row.Stamp.FrameSerial;
                long capQpc = row.Stamp.CaptureBeginQpc;
                bool firstWrite = i.ObservedGeneration == 0u && i.ObservedEpoch == 0UL;
                if (!firstWrite && (i.ObservedGeneration != gen || i.ObservedEpoch != epoch))
                {
                    i.LifetimeIdentityMismatch = 1u;
                    return;
                }

                long common = i.CommonAllValidCount;
                long done = i.InnerCompleteCount;
                long invalid = i.InnerInvalidCount;
                if (common < 0L || done < 0L || invalid < 0L) { i.OverflowMask |= 0x00000004u; return; }
                if (common > long.MaxValue - 1L) { i.OverflowMask |= 0x00000008u; return; }
                long newCommon = common + 1L;
                long newDone = done;
                long newInvalid = invalid;
                if (complete)
                {
                    if (done > long.MaxValue - 1L) { i.OverflowMask |= 0x00000010u; return; }
                    newDone = done + 1L;
                }
                else
                {
                    if (invalid > long.MaxValue - 1L) { i.OverflowMask |= 0x00000011u; return; }
                    newInvalid = invalid + 1L;
                }
                // Partition via safe difference (never unchecked addition).
                if (newCommon - newDone != newInvalid) { i.OverflowMask |= 0x00000012u; return; }
                if (newDone < 0L || newInvalid < 0L) { i.OverflowMask |= 0x00000013u; return; }

                long nP0QcSum = i.P0ConfiguredQualitySumTicks, nP0FlSum = i.P0ActualFlagsSumTicks,
                     nP0RsSum = i.P0RenderStagesSumTicks, nP0ResSum = i.P0ScalarFormattingResidualSumTicks,
                     nP4QcSum = i.P4ConfiguredQualitySumTicks, nP4FlSum = i.P4ActualFlagsSumTicks,
                     nP4RsSum = i.P4RenderStagesSumTicks, nP4ResSum = i.P4ScalarFormattingResidualSumTicks;
                long nP0QcCnt = i.P0ConfiguredQualitySampleCount, nP0FlCnt = i.P0ActualFlagsSampleCount,
                     nP0RsCnt = i.P0RenderStagesSampleCount, nP0ResCnt = i.P0ScalarFormattingResidualSampleCount,
                     nP4QcCnt = i.P4ConfiguredQualitySampleCount, nP4FlCnt = i.P4ActualFlagsSampleCount,
                     nP4RsCnt = i.P4RenderStagesSampleCount, nP4ResCnt = i.P4ScalarFormattingResidualSampleCount;

                if (complete)
                {
                    if (nP0QcSum < 0L || nP0QcCnt < 0L) { i.OverflowMask |= 0x00000100u; return; }
                    if (nP0QcSum > long.MaxValue - p0Qc) { i.OverflowMask |= 0x00000200u; return; }
                    if (nP0FlSum < 0L || nP0FlCnt < 0L) { i.OverflowMask |= 0x00000400u; return; }
                    if (nP0FlSum > long.MaxValue - p0Fl) { i.OverflowMask |= 0x00000800u; return; }
                    if (nP0RsSum < 0L || nP0RsCnt < 0L) { i.OverflowMask |= 0x00001000u; return; }
                    if (nP0RsSum > long.MaxValue - p0Rs) { i.OverflowMask |= 0x00002000u; return; }
                    if (nP0ResSum < 0L || nP0ResCnt < 0L) { i.OverflowMask |= 0x00004000u; return; }
                    if (nP0ResSum > long.MaxValue - p0Res) { i.OverflowMask |= 0x00008000u; return; }
                    if (nP4QcSum < 0L || nP4QcCnt < 0L) { i.OverflowMask |= 0x00010000u; return; }
                    if (nP4QcSum > long.MaxValue - p4Qc) { i.OverflowMask |= 0x00020000u; return; }
                    if (nP4FlSum < 0L || nP4FlCnt < 0L) { i.OverflowMask |= 0x00040000u; return; }
                    if (nP4FlSum > long.MaxValue - p4Fl) { i.OverflowMask |= 0x00080000u; return; }
                    if (nP4RsSum < 0L || nP4RsCnt < 0L) { i.OverflowMask |= 0x00100000u; return; }
                    if (nP4RsSum > long.MaxValue - p4Rs) { i.OverflowMask |= 0x00200000u; return; }
                    if (nP4ResSum < 0L || nP4ResCnt < 0L) { i.OverflowMask |= 0x00400000u; return; }
                    if (nP4ResSum > long.MaxValue - p4Res) { i.OverflowMask |= 0x00800000u; return; }
                    if (nP0QcCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x02000000u; return; }
                    if (nP0FlCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x04000000u; return; }
                    if (nP0RsCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x08000000u; return; }
                    if (nP0ResCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x10000000u; return; }
                    if (nP4QcCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x20000000u; return; }
                    if (nP4FlCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x40000000u; return; }
                    if (nP4RsCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x00000001u; return; }
                    if (nP4ResCnt > long.MaxValue - 1L) { i.OverflowMask |= 0x00000002u; return; }
                    nP0QcSum += p0Qc; nP0FlSum += p0Fl; nP0RsSum += p0Rs; nP0ResSum += p0Res;
                    nP4QcSum += p4Qc; nP4FlSum += p4Fl; nP4RsSum += p4Rs; nP4ResSum += p4Res;
                    nP0QcCnt++; nP0FlCnt++; nP0RsCnt++; nP0ResCnt++;
                    nP4QcCnt++; nP4FlCnt++; nP4RsCnt++; nP4ResCnt++;
                }

                bool firstCommon = common == 0L;
                if (i.FirstCommonFrame != 0UL && frame < i.FirstCommonFrame)
                {
                    // Frame earlier than the established Common first-frame: identity/range violation, sticky failure.
                    i.OverflowMask |= 0x01000000u;
                    return;
                }

                // Single atomic commit: every local has preflighted, so mutate the companion exactly once.
                i.CommonAllValidCount = newCommon;
                i.InnerCompleteCount = newDone;
                i.InnerInvalidCount = newInvalid;
                if (firstWrite)
                {
                    i.ObservedGeneration = gen;
                    i.ObservedEpoch = epoch;
                }
                if (firstCommon)
                {
                    i.FirstCommonFrame = frame;
                    i.FirstCommonCaptureBeginQpc = capQpc;
                }
                i.LastCommonFrame = frame;
                i.LastCommonCaptureBeginQpc = capQpc;

                if (complete)
                {
                    i.P0ConfiguredQualitySumTicks = nP0QcSum; i.P0ConfiguredQualitySampleCount = nP0QcCnt;
                    i.P0ActualFlagsSumTicks = nP0FlSum; i.P0ActualFlagsSampleCount = nP0FlCnt;
                    i.P0RenderStagesSumTicks = nP0RsSum; i.P0RenderStagesSampleCount = nP0RsCnt;
                    i.P0ScalarFormattingResidualSumTicks = nP0ResSum; i.P0ScalarFormattingResidualSampleCount = nP0ResCnt;
                    i.P4ConfiguredQualitySumTicks = nP4QcSum; i.P4ConfiguredQualitySampleCount = nP4QcCnt;
                    i.P4ActualFlagsSumTicks = nP4FlSum; i.P4ActualFlagsSampleCount = nP4FlCnt;
                    i.P4RenderStagesSumTicks = nP4RsSum; i.P4RenderStagesSampleCount = nP4RsCnt;
                    i.P4ScalarFormattingResidualSumTicks = nP4ResSum; i.P4ScalarFormattingResidualSampleCount = nP4ResCnt;
                }
            }
            catch
            {
                try { if (i != null) i.OverflowMask |= 0x80000000u; } catch { }
            }
        }

        private static void SplitQaOwnerPhasePublishRowQa(NC01GpuSplitRowQa row)
        {
            NC01OwnerPhaseCpuWallCacheQa c = null;
            try
            {
                if (row == null || row.Owner == null) return;
                c = row.Owner._qaOwnerPhaseCpuWallCacheQa;
                if (c == null) return;
                if (!QaCacheActive(c)) return;
                if (!QaRowValid(row)) return;
                bool started = false;
                try
                {
                    started = QaBeginPublication(c);
                    if (!started) return;
                    if (!QaBindLifetime(c, row.Generation, row.Stamp.XrEpoch)) return;
                    QaPublishRowScalars(c, row);
                }
                finally
                {
                    if (started) QaEndPublication(c);
                }
            }
            catch
            {
                try { if (c != null) c.OverflowMask |= 0x80000000u; } catch { }
            }
        }

        private static void SplitQaOwnerPhaseRecordOwnerAcceptedQa(NC01GpuSplitRowQa row, NC01GpuPreparedPairV1 dto)
        {
            NC01OwnerPhaseCpuWallCacheQa c = null;
            try
            {
                if (row == null || row.Owner == null) return;
                if (dto == null) return;
                if (row.State != 2) return;
                if (row.OwnerAcceptedRecorded) return;
                c = row.Owner._qaOwnerPhaseCpuWallCacheQa;
                if (c == null) return;
                if (!QaCacheActive(c)) return;
                if (!QaRowValid(row)) return;
                bool started = false;
                try
                {
                    started = QaBeginPublication(c);
                    if (!started) return;
                    if (!QaBindLifetime(c, row.Generation, row.Stamp.XrEpoch)) return;
                    if (!QaTryRecordAcceptedCohort(c, row, dto)) return;
                    row.OwnerAcceptedRecorded = true;
                }
                finally
                {
                    if (started) QaEndPublication(c);
                }
            }
            catch
            {
                try { if (c != null) c.OverflowMask |= 0x80000000u; } catch { }
            }
        }

#endif
        // OWNER_PHASE_PUBLISH_HELPERS_END

#endif
        // OWNER_PHASE_DECL_CACHE_END

        // Pure field-value capture: full LatestReply/LatestOp always; the FIRST Begin packet (even malformed) is kept
        // whole in FirstBeginReply; the FIRST non-zero PreparedTicket packet (even negative rc or wrong shape) is kept
        // whole in FirstGrantReply, written once and never overwritten. No validation, no helpers, no allocation.
        private static void SplitQaSaveActualReply(NC01GpuSplitRowQa row, uint op, NC01GpuSplitNativeReplyV1 reply)
        {
            row.LatestReply = reply;
            row.LatestOp = op;
            bool hadBeginReply = row.HasBeginReply;
            bool hadFirstGrant = row.HasFirstGrant;
            if (!hadBeginReply && op == 1u)
            {
                row.FirstBeginReply = reply;
                row.HasBeginReply = true;
            }
            if (!hadFirstGrant && reply.PreparedTicket != 0UL)
            {
                row.FirstGrantReply = reply;
                row.FirstGrantOp = op;
                row.HasFirstGrant = true;
            }
            row.DuplicateBeginReply = row.DuplicateBeginReply || (op == 1u && hadBeginReply);
            row.ReplyAfterFirstGrant = hadFirstGrant;
        }

        // Saves the actual packet first, then consumes it ONLY through the frozen B1 predicates. Never touches
        // resources, lock masks, owner busy, OriginalToken after first bind, or FirstGrantReply; unknown, wrong-key,
        // duplicate-Begin and post-grant packets reject for THIS round only.
        private static void SplitQaConsumeReply(NC01GpuSplitRowQa row, uint op, NC01GpuSplitNativeReplyV1 reply)
        {
            SplitQaSaveActualReply(row, op, reply);

            bool beginPending = false;
            bool pollPending = false;
            bool grant = false;
            bool cancelAccepted = false;
            bool pollTerminal = false;
            bool beginRollback = false;
            bool unexpected = false;

            if (row.ReplyAfterFirstGrant)
            {
                unexpected = true;
            }
            else if (op == 1u && row.DuplicateBeginReply)
            {
                unexpected = true;
            }
            else if (op == 1u)
            {
                if (SplitQaBeginPending(op, reply, row.Stamp) && !row.HasOriginalToken)
                {
                    row.OriginalToken = reply.Token;
                    row.HasOriginalToken = true;
                    row.EverPending = true;
                    beginPending = true;
                }
                else if (SplitQaActualGrant(op, reply, row.Stamp, row.OriginalToken))
                {
                    grant = true;
                }
                else if (!row.EverPending && !row.HasFirstGrant && SplitQaBeginRollback(op, reply))
                {
                    beginRollback = true;
                }
                else
                {
                    unexpected = true;
                }
            }
            else if (op == 2u)
            {
                if (!row.HasOriginalToken)
                {
                    unexpected = true;
                }
                else if (SplitQaPollPending(op, reply, row.Stamp, row.OriginalToken))
                {
                    pollPending = true;
                }
                else if (SplitQaActualGrant(op, reply, row.Stamp, row.OriginalToken))
                {
                    grant = true;
                }
                else if (SplitQaPollNoGrantTerminal(op, reply, row.Stamp, row.OriginalToken))
                {
                    pollTerminal = true;
                }
                else
                {
                    unexpected = true;
                }
            }
            else if (op == 3u)
            {
                if (!row.HasOriginalToken)
                {
                    unexpected = true;
                }
                else if (SplitQaCancelAccepted(op, reply, row.Stamp, row.OriginalToken))
                {
                    cancelAccepted = true;
                    row.CancelAccepted = true;
                }
                else
                {
                    unexpected = true;
                }
            }
            else
            {
                unexpected = true;
            }

            row.ReplyPending = beginPending || pollPending;
            row.ReplyGrant = grant;
            row.ReplyCancelAccepted = cancelAccepted;
            row.ReplyPollTerminal = pollTerminal;
            row.ReplyBeginRollback = beginRollback;
            row.ReplyUnknown = unexpected;
            if (unexpected)
            {
                row.UnexpectedReply = true;
                row.FaultSeen = true;
            }
        }
#endif
#if VRSIM_QA_D9_BEGIN_POLL
        private struct NC01QaSplitStateCacheV1
        {
            public uint State;
            public uint LastOp;
            public NC01GpuSplitStampV1 Stamp;
            public NC01GpuSplitTokenV1 Token;
            public NC01GpuSplitNativeReplyV1 LastReply;
            public uint LockGrantedMask;
            public uint UnlockAttemptedMask;
            public uint UnlockedMask;
            public uint CallBusy;
            public uint LogicalBusy;
            public bool NativeEntered;
            public bool NativeTerminal;
            public bool StopLatched;
            public bool FaultSeen;
            public bool HasOriginalToken;
            public bool HasFirstGrant;
            public bool CancelAccepted;
            public bool EverPending;
            public bool ReplyGrant;
            public bool ReplyPollTerminal;
            public bool ReplyBeginRollback;
            public bool ReplyCancelAccepted;
            public bool ReplyUnknown;
        }

        private NC01QaSplitStateCacheV1 _qaSplitStateCache;

        private void SplitQaPublishCache(NC01GpuSplitRowQa row)
        {
            if (row != null)
            {
                _qaSplitStateCache.State = (uint)row.State;
                _qaSplitStateCache.LastOp = row.LatestOp;
                _qaSplitStateCache.Stamp = row.Stamp;
                _qaSplitStateCache.Token = row.OriginalToken;
                _qaSplitStateCache.LastReply = row.LatestReply;
                _qaSplitStateCache.LockGrantedMask = row.LockGrantedMask;
                _qaSplitStateCache.UnlockAttemptedMask = row.UnlockAttemptedMask;
                _qaSplitStateCache.UnlockedMask = row.UnlockedMask;
                _qaSplitStateCache.CallBusy = row.CallBusy ? 1u : 0u;
                _qaSplitStateCache.LogicalBusy = (_runtimeCallsBusy != 0 || _captureBusy != 0) ? 1u : 0u;
                _qaSplitStateCache.NativeEntered = row.NativeEntered;
                _qaSplitStateCache.NativeTerminal = row.NativeTerminal;
                _qaSplitStateCache.StopLatched = row.StopLatched;
                _qaSplitStateCache.FaultSeen = row.FaultSeen;
                _qaSplitStateCache.HasOriginalToken = row.HasOriginalToken;
                _qaSplitStateCache.HasFirstGrant = row.HasFirstGrant;
                _qaSplitStateCache.CancelAccepted = row.CancelAccepted;
                _qaSplitStateCache.EverPending = row.EverPending;
                _qaSplitStateCache.ReplyGrant = row.ReplyGrant;
                _qaSplitStateCache.ReplyPollTerminal = row.ReplyPollTerminal;
                _qaSplitStateCache.ReplyBeginRollback = row.ReplyBeginRollback;
                _qaSplitStateCache.ReplyCancelAccepted = row.ReplyCancelAccepted;
                _qaSplitStateCache.ReplyUnknown = row.ReplyUnknown;
            }
            else
            {
                _qaSplitStateCache.CallBusy = 0u;
                _qaSplitStateCache.LogicalBusy = (_runtimeCallsBusy != 0 || _captureBusy != 0) ? 1u : 0u;
            }
        }

        private static void SplitQaUnlockPair(NC01GpuSplitRowQa row)
        {
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
            // OWNER_PHASE_BRACKET_6_BEGIN
            bool ownerPhaseNeedsUnlock6 = row != null && (row.LockGrantedMask & ~row.UnlockAttemptedMask & 3u) != 0u;
            if (ownerPhaseNeedsUnlock6)
                SplitQaOwnerPhaseStartQa(row, 6);
#endif
            if ((row.LockGrantedMask & 2u) != 0u && (row.UnlockAttemptedMask & 2u) == 0u)
            {
                row.UnlockAttemptedMask |= 2u;
                try { row.RightImage.Unlock(); row.UnlockedMask |= 2u; }
                catch (Exception ex) { if (row.RightUnlockFailure == null) row.RightUnlockFailure = ex; }
            }
            if ((row.LockGrantedMask & 1u) != 0u && (row.UnlockAttemptedMask & 1u) == 0u)
            {
                row.UnlockAttemptedMask |= 1u;
                try { row.LeftImage.Unlock(); row.UnlockedMask |= 1u; }
                catch (Exception ex) { if (row.LeftUnlockFailure == null) row.LeftUnlockFailure = ex; }
            }
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
            if (ownerPhaseNeedsUnlock6)
            {
                bool ownerPhaseUnlock6Success =
                    (row.LockGrantedMask & 3u) == 3u &&
                    (row.UnlockAttemptedMask & 3u) == 3u &&
                    (row.UnlockedMask & 3u) == 3u &&
                    row.RightUnlockFailure == null &&
                    row.LeftUnlockFailure == null;
                SplitQaOwnerPhaseEndQa(row, 6, ownerPhaseUnlock6Success);
            }
            // OWNER_PHASE_BRACKET_6_END
#endif
        }

        private static bool SplitQaAllGrantedUnlocked(NC01GpuSplitRowQa row)
        {
            if (row.RightUnlockFailure != null || row.LeftUnlockFailure != null) return false;
            return (row.LockGrantedMask & 3u) == (row.UnlockedMask & 3u);
        }

        private static bool SplitQaCanUnlock(NC01GpuSplitRowQa row)
        {
            return row.NativeTerminal || !row.NativeEntered;
        }

        // Shared final claim/release: clears CallBusy exactly once for the OWNED active claim, and releases BOTH
        // busy registrations plus the active row only on a proven terminal outcome with every granted bit actually
        // unlocked and no stored Unlock failure. Any other outcome retains busy + row (noncommittable drain state).
        private void SplitQaFinishCallClaim(NC01GpuSplitRowQa row)
        {
            lock (_gateLock)
            {
                if (!ReferenceEquals(_qaSplitRow, row) || !ReferenceEquals(row.Owner, this) || !row.CallBusy)
                    return;
                row.CallBusy = false;
                bool terminalState = row.State == 2 || row.State == 3 || row.State == 5 || row.State == 6;
                bool releasable = terminalState
                    && (row.NativeTerminal || !row.NativeEntered)
                    && SplitQaAllGrantedUnlocked(row);
                if (releasable)
                {
                    Volatile.Write(ref _runtimeCallsBusy, 0);
                    Volatile.Write(ref _captureBusy, 0);
                    _qaSplitRow = null;
                }
                SplitQaPublishCache(row);
            }
        }

        // Counts only the actual invocation wall, once per invocation, from the shared finally. Pending gaps between
        // Begin and later Steps are not charged as owner CPU cost.
        private static void SplitQaAccountInvocation(Stopwatch sw, NC01GpuSplitRowQa row)
        {
            if (sw.IsRunning) sw.Stop();
            row.InvocationCpuMs += sw.Elapsed.TotalMilliseconds;
        }

        // Single terminal DTO shape for every Begin/Step exit; reports this call's claim and both logical busy bits.
        private NC01GpuSplitStepV1 SplitQaStepFromRow(NC01GpuSplitRowQa row, uint op)
        {
            return new NC01GpuSplitStepV1
            {
                State = (uint)row.State,
                LastOperationKind = op,
                Stamp = row.Stamp,
                Token = row.OriginalToken,
                LastReply = row.LatestReply,
                LockGrantedMask = row.LockGrantedMask,
                UnlockedMask = row.UnlockedMask,
                CallBusy = 1u,
                LogicalBusy = 1u
            };
        }

        // The delegate's actual packet is saved and consumed as the very next action; only after that does the timing
        // finally account the real callback window, and a callback that throws still yields its own original failure.
        private static NC01GpuSplitNativeReplyV1 SplitQaInvokeBeginProbe(
            NC01GpuSplitRowQa row, Stopwatch probeSw,
            Func<ulong, ulong, NC01GpuSplitNativeReplyV1> begin, ulong left, ulong right)
        {
            NC01GpuSplitNativeReplyV1 actual;
            try
            {
                actual = begin(left, right);
                SplitQaConsumeReply(row, 1u, actual);
            }
            finally { probeSw.Stop(); row.ProbeCpuMs += probeSw.Elapsed.TotalMilliseconds; }
            return actual;
        }

        private static NC01GpuSplitNativeReplyV1 SplitQaInvokeTokenProbe(
            NC01GpuSplitRowQa row, uint op,
            Func<NC01GpuSplitTokenV1, NC01GpuSplitNativeReplyV1> probe, NC01GpuSplitTokenV1 token)
        {
            NC01GpuSplitNativeReplyV1 actual;
            Stopwatch probeSw = Stopwatch.StartNew();
            try
            {
                actual = probe(token);
                SplitQaConsumeReply(row, op, actual);
            }
            finally { probeSw.Stop(); row.ProbeCpuMs += probeSw.Elapsed.TotalMilliseconds; }
            return actual;
        }

        // Pure saved-packet proof through the frozen B1 predicates: no re-Consume, no new callback, no copied reply.
        // op1 accepts the required zero-token immediate grant; op2 demands the exact original key.
        private static bool SplitQaSavedFirstGrantProven(NC01GpuSplitRowQa row)
        {
            return row.HasFirstGrant
                && SplitQaActualGrant(row.FirstGrantOp, row.FirstGrantReply, row.Stamp, row.OriginalToken);
        }

        private static bool SplitQaSavedPollTerminalProven(NC01GpuSplitRowQa row)
        {
            return !row.HasFirstGrant
                && row.HasOriginalToken
                && row.LatestOp == 2u
                && SplitQaPollNoGrantTerminal(2u, row.LatestReply, row.Stamp, row.OriginalToken);
        }

        private static bool SplitQaSavedBeginRollbackProven(NC01GpuSplitRowQa row)
        {
            return !row.HasFirstGrant
                && row.LatestOp == 1u
                && SplitQaBeginRollback(1u, row.LatestReply);
        }

        private static bool SplitQaSavedPendingProven(NC01GpuSplitRowQa row)
        {
            if (row.HasFirstGrant) return false;
            if (row.LatestOp == 1u)
                return SplitQaBeginPending(1u, row.LatestReply, row.Stamp);
            if (row.LatestOp == 2u && row.HasOriginalToken)
                return SplitQaPollPending(2u, row.LatestReply, row.Stamp, row.OriginalToken);
            return false;
        }

        // POST_CALLBACK classification of the exact saved packet only. Unknown or malformed evidence is a real fault:
        // capture closes and the fault stays sticky even when the delegate returned normally without throwing.
        private void SplitQaClassifySavedReply(NC01GpuSplitRowQa row)
        {
            bool grantProven = SplitQaSavedFirstGrantProven(row);
            bool pollTerminalProven = SplitQaSavedPollTerminalProven(row);
            bool rollbackProven = SplitQaSavedBeginRollbackProven(row);
            bool pendingProven = SplitQaSavedPendingProven(row);
            bool cancelAcceptedProven = row.CancelAccepted
                && !row.HasFirstGrant
                && row.HasOriginalToken
                && row.LatestOp == 3u
                && SplitQaCancelAccepted(3u, row.LatestReply, row.Stamp, row.OriginalToken);

            row.ReplyGrant = grantProven;
            row.ReplyPollTerminal = pollTerminalProven;
            row.ReplyBeginRollback = rollbackProven;
            row.ReplyPending = pendingProven;
            row.ReplyCancelAccepted = cancelAcceptedProven;
            row.ReplyUnknown = !(grantProven || pollTerminalProven || rollbackProven || pendingProven || cancelAcceptedProven);
            if (row.ReplyUnknown)
            {
                row.UnexpectedReply = true;
                SplitQaMarkRealFault(row, "OBS_GPU_SPLIT_QA_REPLY_FAULT");
            }
            // Cancellation acceptance alone never terminates: only the three exact terminal proofs do.
            if (grantProven || pollTerminalProven || rollbackProven)
                row.NativeTerminal = true;
        }

        // Every non-input SDK/native/shouldStop fault closes capture before any state classification; a pre-existing
        // fault is never downgraded and the FIRST BodyFailure object is never overwritten by closure or diagnostics.
        // Every real fault is sticky AND permanently latches Stop on this row: capture closes, no prepared commit can
        // ever be made again for this row, and the native terminal proof, locks, both busy bits, original key, first
        // exception and first grant ticket stay exactly as they were. Pre-native INPUT_MATRIX/OCE never calls this.
        private void SplitQaMarkRealFault(NC01GpuSplitRowQa row, string reason)
        {
            row.FaultSeen = true;
            row.StopLatched = true;
            if (!_captureDisabled) DisableCaptures(reason);
            lock (_gateLock)
            {
                if (ReferenceEquals(_qaSplitRow, row) && ReferenceEquals(row.Owner, this))
                    SplitQaPublishCache(row);
            }
        }

        // shouldStop runs outside the lock, then the same row/owner/claim/generation closure is re-verified even when
        // it returned false. A throw keeps the first exception and is a sticky real fault.
        private bool SplitQaStopObserved(NC01GpuSplitRowQa row)
        {
            bool stopReturned;
            try { stopReturned = row.ShouldStop(); }
            catch (Exception ex)
            {
                if (row.BodyFailure == null) row.BodyFailure = ex;
                stopReturned = true;
                SplitQaMarkRealFault(row, "OBS_GPU_SPLIT_QA_SHOULDSTOP_FAULT");
            }
            SplitQaRecheckGateAfterStop(row, stopReturned);
            return stopReturned || row.StopLatched;
        }

        // shouldStop runs outside the lock; the actual gate is re-verified here whether it returned true or false, so a
        // reentrant RequestStop that returned false still latches the row permanently and blocks any prepared commit.
        private void SplitQaRecheckGateAfterStop(NC01GpuSplitRowQa row, bool stopReturned)
        {
            lock (_gateLock)
            {
                if (!ReferenceEquals(_qaSplitRow, row) || !ReferenceEquals(row.Owner, this) || !row.CallBusy)
                    return;
                bool closureObserved = stopReturned
                    || Volatile.Read(ref _stopRequested) != 0
                    || Volatile.Read(ref _admissionOpen) == 0
                    || _captureDisabled
                    || Volatile.Read(ref _hostingState) != 2
                    || Volatile.Read(ref _boundGeneration) != row.Generation;
                if (closureObserved)
                {
                    row.StopLatched = true;
                    SplitQaPublishCache(row);
                }
            }
        }

        private static object SplitQaReflectGetter(object target, string getterName, string tag)
        {
            if (target == null)
                throw new InvalidOperationException("OBS_GPU_SPLIT_QA_BINDING_TARGET_NULL:" + tag + "|" + getterName);
            System.Reflection.PropertyInfo property = FindProperty(target.GetType(), getterName);
            if (property == null)
                throw new InvalidOperationException("OBS_GPU_SPLIT_QA_BINDING_GETTER_MISSING:" + tag + "|" + getterName);
            return property.GetValue(target, null);
        }

        private static object SplitQaReflectField(object target, string fieldName, string tag)
        {
            if (target == null)
                throw new InvalidOperationException("OBS_GPU_SPLIT_QA_BINDING_TARGET_NULL:" + tag + "|" + fieldName);
            System.Reflection.FieldInfo field = target.GetType().GetField(
                fieldName, System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
            if (field == null)
                throw new InvalidOperationException("OBS_GPU_SPLIT_QA_BINDING_FIELD_MISSING:" + tag + "|" + fieldName);
            return field.GetValue(target);
        }

        private static void SplitQaRequireSameBinding(string tag, object frozen, object expected)
        {
            if (frozen == null || expected == null || !ReferenceEquals(frozen, expected))
                throw new InvalidOperationException("OBS_GPU_SPLIT_QA_BINDING_IDENTITY_MISMATCH:" + tag);
        }

        // Verification only: the frozen row references must still be the original live objects and the original
        // binding chain. Current objects are never copied back into the row.
        private void SplitQaVerifyFrozenBindingIdentity(NC01GpuSplitRowQa row)
        {
            SplitQaRequireSameBinding("eye0", _eyes[0], row.LeftEye);
            SplitQaRequireSameBinding("viewport0", _vms[0], row.LeftViewport);
            SplitQaRequireSameBinding("view0", _views[0], row.LeftView);
            SplitQaRequireSameBinding("image0", _images[0], row.LeftImage);
            SplitQaRequireSameBinding("eye1", _eyes[1], row.RightEye);
            SplitQaRequireSameBinding("viewport1", _vms[1], row.RightViewport);
            SplitQaRequireSameBinding("view1", _views[1], row.RightView);
            SplitQaRequireSameBinding("image1", _images[1], row.RightImage);

            SplitQaRequireSameBinding("window0", SplitQaReflectGetter(row.LeftViewport, "Window", "viewport0"), row.LeftEye);
            SplitQaRequireSameBinding("d3dview0", SplitQaReflectGetter(row.LeftViewport, "Direct3DViewportView", "viewport0"), row.LeftView);
            SplitQaRequireSameBinding("d3dimg0", SplitQaReflectField(row.LeftView, "d3dimg", "view0"), row.LeftImage);
            SplitQaRequireSameBinding("camera0", SplitQaReflectGetter(row.LeftEye, "Camera", "eye0"), row.LeftCamera);

            SplitQaRequireSameBinding("window1", SplitQaReflectGetter(row.RightViewport, "Window", "viewport1"), row.RightEye);
            SplitQaRequireSameBinding("d3dview1", SplitQaReflectGetter(row.RightViewport, "Direct3DViewportView", "viewport1"), row.RightView);
            SplitQaRequireSameBinding("d3dimg1", SplitQaReflectField(row.RightView, "d3dimg", "view1"), row.RightImage);
            SplitQaRequireSameBinding("camera1", SplitQaReflectGetter(row.RightEye, "Camera", "eye1"), row.RightCamera);
        }

        // IDENTICAL normal post-guard evidence for Begin and Step, every SDK group granted first and strictly
        // POST_GATE after FINAL_SNAPSHOT/MainAfter equality: stages, quality fingerprint, identity bindings,
        // frozen binding identity, then both camera readbacks against the rendered 16 values.
        private void SplitQaRunPostGuardEvidence(NC01GpuSplitRowQa row, int originalGeneration, NC01MainSnapshotV1 after)
        {
            RequireRuntimeGroup(originalGeneration, row.ShouldStop, "OBS_GPU_POST_GATE", true, "OBS_GPU_STOP_REQUESTED");
            SplitQaRecheckGateAfterStop(row, false);
            if (row.StopLatched || row.FaultSeen || row.CancelAccepted)
                throw new InvalidOperationException("OBS_GPU_SPLIT_QA_STOP_OR_FAULT_BLOCKS_POSTGUARD");

            EnsureEyeStagesMatchBaseline(0);
            EnsureEyeStagesMatchBaseline(1);
            string postFp, postBuildErr;
            string[] postSubset;
            if (!TryBuildRuntimeQualityFingerprint(after.Serialized, out postFp, out postSubset, out postBuildErr))
            { DisableCaptures(postBuildErr); throw new InvalidOperationException(_disabledReason); }
            if (!string.Equals(row.QualityFingerprint, postFp, StringComparison.Ordinal))
            { DisableCaptures("OBS_GPU_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(postSubset)); throw new InvalidOperationException(_disabledReason); }
            VerifyRuntimeIdentityBindings();
            SplitQaVerifyFrozenBindingIdentity(row);

            double[] gateLeftActual16, gateRightActual16;
            double gateLfov, gateLaspect, gateRfov, gateRaspect;
            bool gateLortho, gateRortho;
            ValidateRawEyeProjection(0, row.LeftCamera, row.LeftExpected16, row.LeftFov, row.SourceAspect, "post-gate",
                out gateLeftActual16, out gateLfov, out gateLaspect, out gateLortho);
            ValidateRawEyeProjection(1, row.RightCamera, row.RightExpected16, row.RightFov, row.SourceAspect, "post-gate",
                out gateRightActual16, out gateRfov, out gateRaspect, out gateRortho);
            for (int i = 0; i < 16; i++)
            {
                if (!RawClose(gateLeftActual16[i], row.LeftRendered16[i]))
                    throw new InvalidOperationException("OBS_GPU_SPLIT_QA_POSTGATE_MISMATCH:eye=0|index=" + i.ToString(CultureInfo.InvariantCulture));
                if (!RawClose(gateRightActual16[i], row.RightRendered16[i]))
                    throw new InvalidOperationException("OBS_GPU_SPLIT_QA_POSTGATE_MISMATCH:eye=1|index=" + i.ToString(CultureInfo.InvariantCulture));
            }
        }

        // IDENTICAL exact-grant validation, DTO fill and atomic commit for Begin and Step. Prepared commits only under
        // one gate holding the original grant, row/claim, stamp/generation, saved first grant, proven terminal,
        // every granted lock actually unlocked and no Stop/CancelAccepted/Fault evidence.
        private bool SplitQaTryCommitPreparedGrant(
            NC01GpuSplitRowQa row, int originalGeneration, NC01MainSnapshotV1 after,
            Stopwatch totalSw, out NC01GpuPreparedPairV1 prepared, out string refusal)
        {
            prepared = null;
            refusal = null;

            if (!SplitQaSavedFirstGrantProven(row))
            { refusal = "OBS_GPU_SPLIT_QA_FIRST_GRANT_NOT_PROVEN"; return false; }
            if (!row.NativeTerminal || !SplitQaAllGrantedUnlocked(row) || row.UnlockAttemptedMask != row.LockGrantedMask)
            { refusal = "OBS_GPU_SPLIT_QA_TERMINAL_UNLOCK_NOT_PROVEN"; return false; }

            NC01GpuPreparedPairV1 dto = row.PreparedResult;
            dto.PreparedTicket = row.FirstGrantReply.PreparedTicket;
            dto.Generation = originalGeneration;
            dto.Width = TargetWidth;
            dto.Height = TargetHeight;
            dto.LeftActualView = row.LeftRendered16;
            dto.RightActualView = row.RightRendered16;
            dto.LeftRenderRc = row.LeftRenderRc;
            dto.RightRenderRc = row.RightRenderRc;
            dto.LeftSurfaceHex = row.LeftSurfaceIdentity.ToString("x16", CultureInfo.InvariantCulture);
            dto.RightSurfaceHex = row.RightSurfaceIdentity.ToString("x16", CultureInfo.InvariantCulture);
            dto.MainBefore = row.MainBefore.Serialized;
            dto.MainAfter = after.Serialized;
            row.MainAfter = after;
            dto.MainPreserved = true;
            dto.LeftLocked = (row.LockGrantedMask & 1u) != 0u;
            dto.RightLocked = (row.LockGrantedMask & 2u) != 0u;
            dto.LeftUnlocked = (row.UnlockedMask & 1u) != 0u;
            dto.RightUnlocked = (row.UnlockedMask & 2u) != 0u;
            dto.LeftRenderMs = row.LeftRenderMs;
            dto.RightRenderMs = row.RightRenderMs;
            dto.ProbeMs = row.ProbeCpuMs;
            dto.TotalMs = row.InvocationCpuMs + totalSw.Elapsed.TotalMilliseconds;

            if (SplitQaStopObserved(row) || row.StopLatched || row.CancelAccepted || row.FaultSeen)
            { refusal = "OBS_GPU_SPLIT_QA_STOP_OR_FAULT_BLOCKS_COMMIT"; return false; }

            bool committed;
            string commitReason;
            lock (_gateLock)
            {
                bool cancelDummy;
                committed = RuntimeGroupGrant(originalGeneration, "OBS_GPU_SPLIT_QA_COMMIT", true, out cancelDummy, out commitReason);
                if (committed)
                {
                    committed = ReferenceEquals(_qaSplitRow, row)
                        && ReferenceEquals(row.Owner, this)
                        && row.CallBusy
                        && Volatile.Read(ref _stopRequested) == 0
                        && Volatile.Read(ref _admissionOpen) != 0
                        && !_captureDisabled
                        && Volatile.Read(ref _hostingState) == 2
                        && Volatile.Read(ref _boundGeneration) == originalGeneration
                        && row.Generation == originalGeneration
                        && SplitQaStampValid(row.Stamp, originalGeneration)
                        && row.QualityGeneration == _qualityGeneration
                        && _qualityGeneration == 1
                        && string.Equals(row.QualityFingerprint, _runtimeQualityFingerprint, StringComparison.Ordinal)
                        && SplitQaSavedFirstGrantProven(row)
                        && row.NativeTerminal
                        && row.FirstGrantReply.PreparedTicket == dto.PreparedTicket
                        && row.UnlockAttemptedMask == row.LockGrantedMask
                        && SplitQaAllGrantedUnlocked(row)
                        && !row.StopLatched
                        && !row.CancelAccepted
                        && !row.FaultSeen;
                    if (committed)
                    {
                        _lastBefore = row.MainBefore;
                        _lastAfter = after;
                        row.State = 2;
                        prepared = dto;
                    }
                    else
                    {
                        commitReason = "OBS_GPU_SPLIT_QA_COMMIT_GATE_CLOSED";
                    }
                }
                SplitQaPublishCache(row);
            }
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
            // OWNER_PHASE_ACCEPTED_CALL_BEGIN
            if (committed) { SplitQaOwnerPhaseRecordOwnerAcceptedQa(row, dto); }
            // OWNER_PHASE_ACCEPTED_CALL_END
#endif
            refusal = commitReason;
            return committed;
        }

        // Ownership-evidence failure classification: a state number is never the evidence. Exact native terminal proof
        // plus the actual one-attempt Unlock proof governs release; a native that was entered without proven exact
        // terminal keeps the row, original key, granted locks and both busy registrations for a later drain Step.
        private void SplitQaClassifyFailure(NC01GpuSplitRowQa row, Exception failure, bool inputRejected)
        {
            if (row.BodyFailure == null) row.BodyFailure = failure;

            if (row.LockGrantedMask != 0u && SplitQaCanUnlock(row))
                SplitQaUnlockPair(row);

            bool cleanRelease = SplitQaCanUnlock(row)
                && row.UnlockAttemptedMask == row.LockGrantedMask
                && SplitQaAllGrantedUnlocked(row);
            bool unlockFailure = !SplitQaAllGrantedUnlocked(row) || row.UnlockAttemptedMask != row.LockGrantedMask;

            // Only a pure pre-native input refusal or a genuine cancellation before any native entry stays non-sticky,
            // and only while no fault had already been seen on this row.
            bool nonSticky = !row.NativeEntered && !row.FaultSeen
                && (inputRejected || failure is OperationCanceledException) && !unlockFailure;
            // QA-only diagnostics: append the actual first retained BodyFailure (type + full text incl. stack and
            // inner exception) to the generic reason. Formatting failure must never block the real-fault mark.
            string failureReason = unlockFailure ? "OBS_GPU_SPLIT_QA_UNLOCK_FAULT" : "OBS_GPU_SPLIT_QA_BEGIN_FAULT";
            Exception firstCaught = row.BodyFailure;
            if (firstCaught != null)
            {
                try
                {
                    failureReason = failureReason + " | FIRST_CAUGHT=" + firstCaught.GetType().FullName + " :: " + firstCaught;
                }
                catch
                {
                    failureReason = failureReason + " | FIRST_CAUGHT=UNKNOWN";
                }
            }
            else
            {
                failureReason = failureReason + " | FIRST_CAUGHT=UNKNOWN";
            }
            if (!nonSticky)
                SplitQaMarkRealFault(row, failureReason);

            if (!row.NativeEntered)
            {
                row.State = cleanRelease ? 6 : 4;
            }
            else if (row.NativeTerminal && cleanRelease)
            {
                if (SplitQaSavedFirstGrantProven(row)) row.State = 5;
                else if (SplitQaSavedBeginRollbackProven(row)) row.State = 6;
                else if (SplitQaSavedPollTerminalProven(row)) row.State = 3;
                else row.State = 4;
            }
            else
            {
                row.State = 4;
            }
        }

        public NC01GpuSplitStepV1 BeginPreparedSharedRawPairQa(
            Matrix leftRawView, Matrix rightRawView,
            double leftSourceFovYDeg, double rightSourceFovYDeg,
            double sourceAspect, bool perspective, int generation,
            NC01GpuSplitStampV1 stamp, Func<bool> shouldStop,
            Func<ulong, ulong, NC01GpuSplitNativeReplyV1> begin,
            Func<NC01GpuSplitTokenV1, NC01GpuSplitNativeReplyV1> poll,
            Func<NC01GpuSplitTokenV1, NC01GpuSplitNativeReplyV1> cancel,
            out NC01GpuPreparedPairV1 prepared)
        {
            BeginPreparedSharedRawPairCoreQa(
                leftRawView, rightRawView, leftSourceFovYDeg, rightSourceFovYDeg,
                sourceAspect, perspective, generation, stamp, shouldStop, begin, poll, cancel, out prepared);
            return ReadPreparedSharedRawPairQaState();
        }

        public NC01GpuSplitStepV1 StepPreparedSharedRawPairQa(
            int originalGeneration, bool drainOnly, out NC01GpuPreparedPairV1 prepared)
        {
            StepPreparedSharedRawPairCoreQa(originalGeneration, drainOnly, out prepared);
            return ReadPreparedSharedRawPairQaState();
        }

        private NC01GpuSplitStepV1 BeginPreparedSharedRawPairCoreQa(
            Matrix leftRawView, Matrix rightRawView,
            double leftSourceFovYDeg, double rightSourceFovYDeg,
            double sourceAspect, bool perspective, int generation,
            NC01GpuSplitStampV1 stamp, Func<bool> shouldStop,
            Func<ulong, ulong, NC01GpuSplitNativeReplyV1> begin,
            Func<NC01GpuSplitTokenV1, NC01GpuSplitNativeReplyV1> poll,
            Func<NC01GpuSplitTokenV1, NC01GpuSplitNativeReplyV1> cancel,
            out NC01GpuPreparedPairV1 prepared)
        {
            prepared = null;
            if (begin == null) throw new ArgumentNullException("begin");
            if (poll == null) throw new ArgumentNullException("poll");
            if (cancel == null) throw new ArgumentNullException("cancel");
            if (shouldStop == null) throw new ArgumentNullException("shouldStop");
            if (!_runtimeBackend) throw new InvalidOperationException("OBS_GPU_RUNTIME_BACKEND_REQUIRED:runtimeBackend=0");
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            if (!perspective) throw new InvalidOperationException("OBS_GPU_PERSPECTIVE_REQUIRED:perspective=0");
            RequireRawFovY(0, leftSourceFovYDeg);
            RequireRawFovY(1, rightSourceFovYDeg);
            RequireRawSourceAspect(sourceAspect);
            if (!SplitQaStampValid(stamp, generation))
                throw new InvalidOperationException("OBS_GPU_SPLIT_QA_STAMP_INVALID");
            VerifyAccess();

            NC01GpuSplitRowQa row = new NC01GpuSplitRowQa(this, stamp);
            row.Begin = begin;
            row.Poll = poll;
            row.Cancel = cancel;
            row.ShouldStop = shouldStop;
            row.LeftFov = leftSourceFovYDeg;
            row.RightFov = rightSourceFovYDeg;
            row.SourceAspect = sourceAspect;

            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) throw new InvalidOperationException("OBS_GPU_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _stopRequested) != 0) throw new OperationCanceledException("OBS_GPU_STOP_REQUESTED:generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _admissionOpen) == 0) throw new InvalidOperationException("OBS_GPU_ADMISSION_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _hostingState) != 2) throw new InvalidOperationException("NC01_NOT_HOSTED");
                if (_captureDisabled) throw new InvalidOperationException("NC01_CAPTURE_DISABLED:" + _disabledReason);
                if (_qualityGeneration != 1) throw new InvalidOperationException("OBS_GPU_QUALITY_NOT_FROZEN:generation=" + _qualityGeneration.ToString(CultureInfo.InvariantCulture));
                if (_runtimeCallsBusy != 0) throw new InvalidOperationException("OBS_GPU_RUNTIME_CALL_BUSY:splitQa|generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (_captureBusy != 0) throw new InvalidOperationException("NC01_CAPTURE_BUSY");
                if (_qaSplitRow != null) throw new InvalidOperationException("OBS_GPU_SPLIT_QA_ACTIVE_ROW_EXISTS");
                _qaSplitRow = row;
                row.CallBusy = true;
                _runtimeCallsBusy = 1;
                _captureBusy = 1;
                SplitQaPublishCache(row);
            }

            Stopwatch totalSw = Stopwatch.StartNew();
            ICamera camLeft = null, camRight = null;
            IntPtr leftScene = IntPtr.Zero, rightScene = IntPtr.Zero;
            double[] leftActual16 = null, rightActual16 = null;
            double leftActualFov, leftActualAspect, rightActualFov, rightActualAspect;
            bool leftActualOrtho, rightActualOrtho;

            try
            {
                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_INPUT_MATRIX", true, "OBS_GPU_STOP_REQUESTED");
                double[] left16;
                double[] right16;
                try
                {
                    left16 = RequireRawView16Finite(0, leftRawView);
                    right16 = RequireRawView16Finite(1, rightRawView);
                    Array.Copy(left16, row.LeftExpected16, 16);
                    Array.Copy(right16, row.RightExpected16, 16);
                }
                catch (Exception inputFailure)
                {
                    SplitQaClassifyFailure(row, inputFailure, true);
                    return SplitQaStepFromRow(row, 1u);
                }

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_PREPARE", true, "OBS_GPU_STOP_REQUESTED");
                VerifyRuntimeIdentityBindings();
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_0_BEGIN
                SplitQaOwnerPhaseStartQa(row, 0);
                row.P0InnerOpen = 1;
                row.MainBefore = MainSnapshotCore(row, 0);
                row.P0InnerOpen = 0;
                SplitQaOwnerPhaseEndQa(row, 0, true);
                // OWNER_PHASE_BRACKET_0_END
#else
                row.MainBefore = MainSnapshotCore();
#endif

                string readyReason;
                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_READY0", true, "OBS_GPU_STOP_REQUESTED");
                if (!TryEyeReady(0, out readyReason) || !TryConfirmEyeStageBaseline(0, out readyReason))
                    throw new InvalidOperationException("OBS_GPU_SPLIT_QA_PREPARE_REFUSED:eye=0|reason=" + readyReason);
                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_READY1", true, "OBS_GPU_STOP_REQUESTED");
                if (!TryEyeReady(1, out readyReason) || !TryConfirmEyeStageBaseline(1, out readyReason))
                    throw new InvalidOperationException("OBS_GPU_SPLIT_QA_PREPARE_REFUSED:eye=1|reason=" + readyReason);

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_QUALITY_BUILD", true, "OBS_GPU_STOP_REQUESTED");
                string pairFingerprint, pairBuildError;
                string[] pairSubset;
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_1_BEGIN
                SplitQaOwnerPhaseStartQa(row, 1);
#endif
                if (!TryBuildRuntimeQualityFingerprint(row.MainBefore.Serialized, out pairFingerprint, out pairSubset, out pairBuildError))
                { DisableCaptures(pairBuildError); throw new InvalidOperationException(_disabledReason); }
                if (!string.Equals(_runtimeQualityFingerprint, pairFingerprint, StringComparison.Ordinal))
                { DisableCaptures("OBS_GPU_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(pairSubset)); throw new InvalidOperationException(_disabledReason); }
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                SplitQaOwnerPhaseEndQa(row, 1, true);
                // OWNER_PHASE_BRACKET_1_END
#endif
                row.QualityGeneration = _qualityGeneration;
                row.QualityFingerprint = pairFingerprint;

#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_2_BEGIN
                SplitQaOwnerPhaseStartQa(row, 2);
#endif
                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_LEFT_CAMERA", true, "OBS_GPU_STOP_REQUESTED");
                EnsureEyeStagesMatchBaseline(0);
                camLeft = _eyes[0].Camera;
                camLeft.IsOrthographic = false;
                camLeft.AspectRatio = sourceAspect;
                camLeft.FieldOfViewY = leftSourceFovYDeg;
                camLeft.ViewMatrix = leftRawView;
                camLeft.Update();
                ValidateRawEyeProjection(0, camLeft, row.LeftExpected16, leftSourceFovYDeg, sourceAspect, "post-update",
                    out leftActual16, out leftActualFov, out leftActualAspect, out leftActualOrtho);
                leftScene = RequireScenePointer(0);
                row.LeftCamera = camLeft;
                row.LeftEye = _eyes[0];
                row.LeftViewport = _vms[0];
                row.LeftView = _views[0];
                row.LeftImage = _images[0];
                row.LeftSurfaceIdentity = unchecked((ulong)leftScene.ToInt64());

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_RIGHT_CAMERA", true, "OBS_GPU_STOP_REQUESTED");
                EnsureEyeStagesMatchBaseline(1);
                camRight = _eyes[1].Camera;
                camRight.IsOrthographic = false;
                camRight.AspectRatio = sourceAspect;
                camRight.FieldOfViewY = rightSourceFovYDeg;
                camRight.ViewMatrix = rightRawView;
                camRight.Update();
                ValidateRawEyeProjection(1, camRight, row.RightExpected16, rightSourceFovYDeg, sourceAspect, "post-update",
                    out rightActual16, out rightActualFov, out rightActualAspect, out rightActualOrtho);
                rightScene = RequireScenePointer(1);
                row.RightCamera = camRight;
                row.RightEye = _eyes[1];
                row.RightViewport = _vms[1];
                row.RightView = _views[1];
                row.RightImage = _images[1];
                row.RightSurfaceIdentity = unchecked((ulong)rightScene.ToInt64());
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                SplitQaOwnerPhaseEndQa(row, 2, true);
                // OWNER_PHASE_BRACKET_2_END
#endif

#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_3_BEGIN
                SplitQaOwnerPhaseStartQa(row, 3);
#endif
                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_LEFT_LOCK", true, "OBS_GPU_STOP_REQUESTED");
                bool leftLocked = row.LeftImage.TryLock(new Duration(TimeSpan.FromMilliseconds(10)));
                if (!leftLocked) throw new InvalidOperationException("OBS_GPU_DX_LOCK_TIMEOUT:eye=0");
                row.LockGrantedMask |= 1u;

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_RIGHT_LOCK", true, "OBS_GPU_STOP_REQUESTED");
                bool rightLocked = row.RightImage.TryLock(new Duration(TimeSpan.FromMilliseconds(10)));
                if (!rightLocked) throw new InvalidOperationException("OBS_GPU_DX_LOCK_TIMEOUT:eye=1");
                row.LockGrantedMask |= 2u;
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                SplitQaOwnerPhaseEndQa(row, 3, true);
                // OWNER_PHASE_BRACKET_3_END
#endif

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_LEFT_RENDER", true, "OBS_GPU_STOP_REQUESTED");
                Stopwatch leftRenderSw = Stopwatch.StartNew();
                int leftRenderRc = _app.SurfaceRender(row.LeftEye);
                leftRenderSw.Stop();
                row.LeftRenderRc = leftRenderRc;
                row.LeftRenderMs = leftRenderSw.Elapsed.TotalMilliseconds;
                if (leftRenderRc < 0) throw new InvalidOperationException("OBS_GPU_SURFACE_RENDER_FAILED:eye=0|actualrc=" + leftRenderRc.ToString(CultureInfo.InvariantCulture));
                if (RequireScenePointer(0) != leftScene) throw new InvalidOperationException("OBS_GPU_SPLIT_QA_SCENE_IDENTITY_CHANGED:eye=0");
                EnsureEyeStagesMatchBaseline(0);
                double[] postRenderLeftActual16;
                ValidateRawEyeProjection(0, camLeft, row.LeftExpected16, leftSourceFovYDeg, sourceAspect, "post-render",
                    out postRenderLeftActual16, out leftActualFov, out leftActualAspect, out leftActualOrtho);
                Array.Copy(postRenderLeftActual16, row.LeftRendered16, 16);

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_RIGHT_RENDER", true, "OBS_GPU_STOP_REQUESTED");
                Stopwatch rightRenderSw = Stopwatch.StartNew();
                int rightRenderRc = _app.SurfaceRender(row.RightEye);
                rightRenderSw.Stop();
                row.RightRenderRc = rightRenderRc;
                row.RightRenderMs = rightRenderSw.Elapsed.TotalMilliseconds;
                if (rightRenderRc < 0) throw new InvalidOperationException("OBS_GPU_SURFACE_RENDER_FAILED:eye=1|actualrc=" + rightRenderRc.ToString(CultureInfo.InvariantCulture));
                if (RequireScenePointer(1) != rightScene) throw new InvalidOperationException("OBS_GPU_SPLIT_QA_SCENE_IDENTITY_CHANGED:eye=1");
                EnsureEyeStagesMatchBaseline(1);
                double[] postRenderRightActual16;
                ValidateRawEyeProjection(1, camRight, row.RightExpected16, rightSourceFovYDeg, sourceAspect, "post-render",
                    out postRenderRightActual16, out rightActualFov, out rightActualAspect, out rightActualOrtho);
                Array.Copy(postRenderRightActual16, row.RightRendered16, 16);

                RequireRuntimeGroup(generation, shouldStop, "OBS_GPU_CALLBACK", true, "OBS_GPU_STOP_REQUESTED");
                SplitQaRecheckGateAfterStop(row, false);
                if (row.StopLatched)
                    throw new OperationCanceledException("OBS_GPU_STOP_REQUESTED:splitQaBeforeNative");
                if (RequireScenePointer(0) != leftScene || RequireScenePointer(1) != rightScene)
                    throw new InvalidOperationException("OBS_GPU_SPLIT_QA_SCENE_CHANGED_BEFORE_BEGIN");

                Stopwatch beginProbeSw = Stopwatch.StartNew();
                row.NativeEntered = true;
                NC01GpuSplitNativeReplyV1 beginReply = SplitQaInvokeBeginProbe(
                    row, beginProbeSw, begin, unchecked((ulong)leftScene.ToInt64()), unchecked((ulong)rightScene.ToInt64()));
                SplitQaClassifySavedReply(row);
                bool beginStopObserved = SplitQaStopObserved(row);

                if (row.ReplyPending && !row.FaultSeen)
                {
                    // Normal Stop drain: Pending keeps the granted locks, both busy bits and the row for later Steps.
                    row.State = 1;
                    return SplitQaStepFromRow(row, 1u);
                }

                if (row.ReplyPending || row.ReplyUnknown)
                {
                    // Pending with a permanent fault, or any unknown/malformed Begin packet: no Unlock, row retained.
                    row.State = 4;
                    if (row.BodyFailure == null)
                        row.BodyFailure = new InvalidOperationException("OBS_GPU_SPLIT_QA_BEGIN_REPLY_RETAINED:op=1|status=" + beginReply.Status.ToString(CultureInfo.InvariantCulture));
                    return SplitQaStepFromRow(row, 1u);
                }

                if (row.NativeTerminal)
                {
                    SplitQaUnlockPair(row);
                    if (!SplitQaAllGrantedUnlocked(row) || row.UnlockAttemptedMask != row.LockGrantedMask)
                    {
                        row.State = 4;
                        SplitQaMarkRealFault(row, "OBS_GPU_SPLIT_QA_UNLOCK_FAULT");
                        return SplitQaStepFromRow(row, 1u);
                    }

                    if (SplitQaSavedBeginRollbackProven(row) && !row.HasFirstGrant)
                    {
                        row.State = 6;
                        return SplitQaStepFromRow(row, 1u);
                    }

                    if (!SplitQaSavedFirstGrantProven(row))
                    {
                        row.State = 4;
                        if (row.BodyFailure == null)
                            row.BodyFailure = new InvalidOperationException("OBS_GPU_SPLIT_QA_BEGIN_REPLY_RETAINED:op=1|status=" + beginReply.Status.ToString(CultureInfo.InvariantCulture));
                        return SplitQaStepFromRow(row, 1u);
                    }

                    if (beginStopObserved || row.StopLatched || row.FaultSeen || row.CancelAccepted)
                    {
                        row.State = 5;
                        return SplitQaStepFromRow(row, 1u);
                    }

                    // Exact grant + all unlocked + no fault/stop → post-guard, FINAL_SNAPSHOT first, then POST_GATE.
                    RequireRuntimeGroup(generation, row.ShouldStop, "OBS_GPU_FINAL_SNAPSHOT", true, "OBS_GPU_STOP_REQUESTED");
                    if (SplitQaStopObserved(row))
                    {
                        row.State = 5;
                        return SplitQaStepFromRow(row, 1u);
                    }

#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                    // OWNER_PHASE_BRACKET_4_BEGIN
                    SplitQaOwnerPhaseStartQa(row, 4);
                    row.P4InnerOpen = 1;
                    NC01MainSnapshotV1 after = MainSnapshotCore(row, 4);
                    row.P4InnerOpen = 0;
                    SplitQaOwnerPhaseEndQa(row, 4, true);
                    // OWNER_PHASE_BRACKET_4_END
#else
                    NC01MainSnapshotV1 after = MainSnapshotCore();
#endif
                    if (!after.StrictEquals(row.MainBefore))
                        throw new InvalidOperationException("OBS_GPU_SPLIT_QA_SNAPSHOT_MISMATCH");

#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                    // OWNER_PHASE_BRACKET_5_BEGIN
                    SplitQaOwnerPhaseStartQa(row, 5);
#endif
                    SplitQaRunPostGuardEvidence(row, generation, after);
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                    SplitQaOwnerPhaseEndQa(row, 5, true);
                    // OWNER_PHASE_BRACKET_5_END
#endif

                    NC01GpuPreparedPairV1 committedPrepared;
                    string commitRefusal;
                    if (!SplitQaTryCommitPreparedGrant(row, generation, after, totalSw, out committedPrepared, out commitRefusal))
                    {
                        row.State = 5;
                        if (row.BodyFailure == null)
                            row.BodyFailure = new InvalidOperationException(commitRefusal ?? "OBS_GPU_SPLIT_QA_COMMIT_REFUSED");
                        return SplitQaStepFromRow(row, 1u);
                    }

                    prepared = committedPrepared;
                    return SplitQaStepFromRow(row, 1u);
                }

                row.State = 4;
                if (row.BodyFailure == null)
                    row.BodyFailure = new InvalidOperationException("OBS_GPU_SPLIT_QA_UNRESOLVED");
                return SplitQaStepFromRow(row, 1u);
            }
            catch (Exception ex)
            {
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_ABORT_BEGIN
                SplitQaOwnerPhaseAbortQa(row);
                // OWNER_PHASE_BRACKET_ABORT_END
#endif
                SplitQaClassifyFailure(row, ex, false);
                return SplitQaStepFromRow(row, 1u);
            }
            finally
            {
                SplitQaAccountInvocation(totalSw, row);
                SplitQaFinishCallClaim(row);
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_PUBLISH_BEGIN
                SplitQaOwnerPhasePublishRowQa(row);
                // OWNER_PHASE_BRACKET_PUBLISH_END
#endif
            }
        }

        private NC01GpuSplitStepV1 StepPreparedSharedRawPairCoreQa(
            int originalGeneration, bool drainOnly, out NC01GpuPreparedPairV1 prepared)
        {
            prepared = null;
            VerifyAccess();

            NC01GpuSplitRowQa row;
            lock (_gateLock)
            {
                row = _qaSplitRow;
                if (row == null)
                {
                    return new NC01GpuSplitStepV1
                    {
                        State = _qaSplitStateCache.State,
                        LastOperationKind = _qaSplitStateCache.LastOp,
                        Stamp = _qaSplitStateCache.Stamp,
                        Token = _qaSplitStateCache.Token,
                        LastReply = _qaSplitStateCache.LastReply,
                        LockGrantedMask = _qaSplitStateCache.LockGrantedMask,
                        UnlockedMask = _qaSplitStateCache.UnlockedMask,
                        CallBusy = _qaSplitStateCache.CallBusy,
                        LogicalBusy = _qaSplitStateCache.LogicalBusy
                    };
                }
                int bound = Volatile.Read(ref _boundGeneration);
                if (originalGeneration != row.Generation || row.Generation != bound)
                    throw new InvalidOperationException("OBS_GPU_SPLIT_QA_GEN_MISMATCH:row=" + FormatInvariant(row.Generation) + "|bound=" + FormatInvariant(bound) + "|original=" + originalGeneration.ToString(CultureInfo.InvariantCulture));
                if (row.CallBusy)
                    throw new InvalidOperationException("OBS_GPU_SPLIT_QA_CALL_BUSY");
                if (drainOnly)
                {
                    bool drainOk = Volatile.Read(ref _stopRequested) != 0 && Volatile.Read(ref _admissionOpen) == 0;
                    if (!drainOk)
                        throw new InvalidOperationException("OBS_GPU_SPLIT_QA_DRAIN_PRECONDITION");
                }
                row.CallBusy = true;
                SplitQaPublishCache(row);
            }

            Stopwatch totalSw = Stopwatch.StartNew();

            // A fault already recorded on this row is sticky history, including a synthetic no-key row: it closes
            // capture again, latches Stop, and never erases the row, original key, granted locks or both busy bits.
            if (row.FaultSeen)
            {
                SplitQaMarkRealFault(row, "OBS_GPU_SPLIT_QA_HISTORICAL_FAULT");
                SplitQaRecheckGateAfterStop(row, true);
            }
            SplitQaStopObserved(row);

            try
            {
                uint currentOp = 0u;

                // Closure permits only the original-key native drain; no callback is issued once any first grant exists.
                bool needCancel = (row.StopLatched || row.FaultSeen || Volatile.Read(ref _admissionOpen) == 0)
                    && row.HasOriginalToken && !row.CancelAccepted && !row.HasFirstGrant && !row.NativeTerminal;

                if (needCancel)
                {
                    currentOp = 3u;
                    SplitQaInvokeTokenProbe(row, 3u, row.Cancel, row.OriginalToken);
                    SplitQaClassifySavedReply(row);
                    // Per-callback observation: a Stop or ShouldStop fault latches before the next callback or branch.
                    SplitQaStopObserved(row);
                }

                if (row.HasOriginalToken && !row.HasFirstGrant && !row.NativeTerminal)
                {
                    currentOp = 2u;
                    SplitQaInvokeTokenProbe(row, 2u, row.Poll, row.OriginalToken);
                    SplitQaClassifySavedReply(row);
                    SplitQaStopObserved(row);
                }

                bool stepStopObserved = SplitQaStopObserved(row);

                if (row.HasFirstGrant && !SplitQaSavedFirstGrantProven(row) && !row.NativeTerminal)
                {
                    // Malformed or unproven non-zero first ticket: sticky closure, row/key/packet retained for drain.
                    row.State = 4;
                    if (row.BodyFailure == null)
                        row.BodyFailure = new InvalidOperationException("OBS_GPU_SPLIT_QA_STEP_FIRST_GRANT_UNPROVEN");
                    SplitQaMarkRealFault(row, "OBS_GPU_SPLIT_QA_STEP_FIRST_GRANT_UNPROVEN");
                    return SplitQaStepFromRow(row, currentOp);
                }

                if (!row.NativeTerminal && !row.ReplyPollTerminal && !row.ReplyBeginRollback)
                {
                    // Exact Pending holds the granted locks and both busy bits even when Stop is latched or a Cancel was
                    // accepted; only a prior fault turns Pending into the retained State4. Stop still blocks Prepared.
                    if (row.ReplyPending && !row.FaultSeen)
                    {
                        row.State = 1;
                        return SplitQaStepFromRow(row, currentOp);
                    }
                    row.State = 4;
                    if (row.BodyFailure == null)
                        row.BodyFailure = new InvalidOperationException("OBS_GPU_SPLIT_QA_STEP_NONTERMINAL_UNRESOLVED");
                    return SplitQaStepFromRow(row, currentOp);
                }

                if (row.NativeTerminal || !row.NativeEntered)
                {
                    SplitQaUnlockPair(row);
                    if (!SplitQaAllGrantedUnlocked(row) || row.UnlockAttemptedMask != row.LockGrantedMask)
                    {
                        row.State = 4;
                        SplitQaMarkRealFault(row, "OBS_GPU_SPLIT_QA_UNLOCK_FAULT");
                        return SplitQaStepFromRow(row, currentOp);
                    }
                }

                if (SplitQaSavedFirstGrantProven(row) && row.NativeTerminal)
                {
                    if (stepStopObserved || row.StopLatched || row.CancelAccepted || row.FaultSeen)
                    {
                        row.State = 5;
                        return SplitQaStepFromRow(row, currentOp);
                    }

                    RequireRuntimeGroup(row.Generation, row.ShouldStop, "OBS_GPU_FINAL_SNAPSHOT", true, "OBS_GPU_STOP_REQUESTED");
                    if (SplitQaStopObserved(row))
                    {
                        row.State = 5;
                        return SplitQaStepFromRow(row, currentOp);
                    }

#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                    // OWNER_PHASE_BRACKET_4_BEGIN
                    SplitQaOwnerPhaseStartQa(row, 4);
                    row.P4InnerOpen = 1;
                    NC01MainSnapshotV1 after = MainSnapshotCore(row, 4);
                    row.P4InnerOpen = 0;
                    SplitQaOwnerPhaseEndQa(row, 4, true);
                    // OWNER_PHASE_BRACKET_4_END
#else
                    NC01MainSnapshotV1 after = MainSnapshotCore();
#endif
                    if (!after.StrictEquals(row.MainBefore))
                        throw new InvalidOperationException("OBS_GPU_SPLIT_QA_SNAPSHOT_MISMATCH");

#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                    // OWNER_PHASE_BRACKET_5_BEGIN
                    SplitQaOwnerPhaseStartQa(row, 5);
#endif
                    SplitQaRunPostGuardEvidence(row, row.Generation, after);
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                    SplitQaOwnerPhaseEndQa(row, 5, true);
                    // OWNER_PHASE_BRACKET_5_END
#endif

                    NC01GpuPreparedPairV1 committedPrepared;
                    string commitRefusal;
                    if (!SplitQaTryCommitPreparedGrant(row, row.Generation, after, totalSw, out committedPrepared, out commitRefusal))
                    {
                        row.State = 5;
                        if (row.BodyFailure == null)
                            row.BodyFailure = new InvalidOperationException(commitRefusal ?? "OBS_GPU_SPLIT_QA_COMMIT_REFUSED");
                        return SplitQaStepFromRow(row, currentOp);
                    }

                    prepared = committedPrepared;
                    return SplitQaStepFromRow(row, currentOp);
                }
                else if (row.ReplyPollTerminal)
                {
                    row.State = 3;
                    return SplitQaStepFromRow(row, currentOp);
                }
                else if (row.ReplyBeginRollback)
                {
                    row.State = 6;
                    return SplitQaStepFromRow(row, currentOp);
                }
                else
                {
                    row.State = 4;
                    if (row.BodyFailure == null)
                        row.BodyFailure = new InvalidOperationException("OBS_GPU_SPLIT_QA_STEP_UNRESOLVED_TERMINAL_EVIDENCE");
                    return SplitQaStepFromRow(row, currentOp);
                }
            }
            catch (Exception ex)
            {
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_ABORT_BEGIN
                SplitQaOwnerPhaseAbortQa(row);
                // OWNER_PHASE_BRACKET_ABORT_END
#endif
                SplitQaClassifyFailure(row, ex, false);
                return SplitQaStepFromRow(row, 0u);
            }
            finally
            {
                SplitQaAccountInvocation(totalSw, row);
                SplitQaFinishCallClaim(row);
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
                // OWNER_PHASE_BRACKET_PUBLISH_BEGIN
                SplitQaOwnerPhasePublishRowQa(row);
                // OWNER_PHASE_BRACKET_PUBLISH_END
#endif
            }
        }

        public NC01GpuSplitStepV1 ReadPreparedSharedRawPairQaState()
        {
            NC01QaSplitStateCacheV1 snap;
            lock (_gateLock)
            {
                snap = _qaSplitStateCache;
            }
            return new NC01GpuSplitStepV1
            {
                State = snap.State,
                LastOperationKind = snap.LastOp,
                Stamp = snap.Stamp,
                Token = snap.Token,
                LastReply = snap.LastReply,
                LockGrantedMask = snap.LockGrantedMask,
                UnlockedMask = snap.UnlockedMask,
                CallBusy = snap.CallBusy,
                LogicalBusy = snap.LogicalBusy
            };
        }
#endif
    }
}
