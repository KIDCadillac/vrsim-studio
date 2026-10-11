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
    }
}
