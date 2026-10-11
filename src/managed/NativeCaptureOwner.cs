using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using VisualComponents.Create3D;

namespace VRSIM
{
    // Sole recording/renderer owner for native capture. Caller must already be on the
    // SIM UI thread; this class never dispatches, never touches scene Save / simulation / XR.
    internal sealed class NativeCaptureOwner : IDisposable
    {
        private const int MaxDimension = 2048;
        private const double Tolerance = 1e-6;
        private const int BytesPerPixel = 4;
        private const int MaxDiagnosticChars = 8192;

        private readonly IApplication app;
        private readonly IViewport viewport;
        private readonly IRenderService service;
        private readonly IWindow3D win;
        private readonly ICamera cam;
        private readonly int width;
        private readonly int height;
        private readonly int payload;

        // Restore target: the constructor commits the initial snapshot and every Capture
        // refreshes it to the LIVE desktop state BEFORE any SDK write, so user camera moves,
        // zoom and viewport adjustments between captures are kept instead of rolled back.
        // Refresh is all-or-nothing: every value is read into locals and validated first;
        // orig* is committed only on complete success (never a partial refresh).
        private int origWidth;
        private int origHeight;
        private Matrix origViewMatrix;
        private double[] origViewRaw;
        private double origFovY;
        private double origAspect;
        private double origGaze;
        private bool origOrtho;
        private bool origRenderingEnabled;

        private readonly bool useStereoViewportRecording;

        private bool captureInProgress;
        private bool mutated;

        // Tracks whether our own SDK mutations still need a desktop write-back. Set before
        // the first SDK write in Begin/Capture, cleared only when a restore pass completes
        // with no new errors. When false, Exit/Restore must NOT re-apply the stored snapshot
        // — the user may have legitimately moved/resized/changed FOV since the last capture.
        private bool desktopRestoreNeeded;
        private bool begun;
        private bool restored;
        private bool disposed;
        private bool nativeRecordingPending;
        private bool viewportRecordingPending;
        private bool stickyNativeCloseFailed;
        private bool stickyViewportCloseFailed;
        private bool stickyDesktopRestoreFailed;
        private readonly StringBuilder diagnostic = new StringBuilder();

        internal bool Begun { get => begun; }
        internal bool RestoreVerified { get; private set; }
        internal ulong CaptureCount { get; private set; }
        internal long LastCaptureMs { get; private set; }
        internal int LastActualWidth { get; private set; }
        internal int LastActualHeight { get; private set; }
        internal int LastPayloadBytes { get; private set; }
        internal double LastActualAspectRatio { get; private set; }
        internal double LastActualFieldOfViewY { get; private set; }
        internal Matrix LastActualRawView { get; private set; }

        // Stereo-pair candidate state. LastStereoPairMs covers the paired capture AND cleanup
        // only (the stopwatch starts after the live desktop snapshot refresh, so snapshot
        // getter time is excluded); it is separate from the single-capture LastCaptureMs and
        // the per-stage timings, which stereo capture does not touch. NaN = never completed.
        internal double LastStereoPairMs { get; private set; }
        internal Matrix LastStereoLeftRaw { get; private set; }
        internal Matrix LastStereoRightRaw { get; private set; }
        internal double LastStereoLeftFov { get; private set; }
        internal double LastStereoRightFov { get; private set; }
        internal int LastStereoWidth { get; private set; }
        internal int LastStereoHeight { get; private set; }
        internal double LastStereoAspect { get; private set; }
        internal ulong StereoPairCount { get; private set; }

        // Attempted CPU duration (Stopwatch delta) of each individual close call in the stereo
        // cleanup, timed separately because LastStereoPairMs alone cannot distinguish SDK
        // vs viewport close cost. Measured around the attempt regardless of outcome; NOT a
        // success indicator or GPU timestamp. NaN = stage not reached on the last pair.
        internal double LastStereoNativeCloseMs { get; private set; }
        internal double LastStereoViewportCloseMs { get; private set; }

        // Per-capture substage timing (milliseconds, monotonic Stopwatch deltas). Reset to
        // NaN/false after the live snapshot refresh and before any SDK mutation; a stage
        // that never completed stays NaN (unknown). Sums cover LastTotalPreciseMs within
        // stopwatch-read overhead. LastCaptureMs remains the rounded total excluding input
        // and snapshot validation.
        internal double LastGateMs { get; private set; }
        internal double LastResizeMs { get; private set; }
        internal double LastCameraMs { get; private set; }
        internal double LastRecordingEnableMs { get; private set; }
        internal double LastBufferReadbackMs { get; private set; }
        internal double LastPrivateCopyMs { get; private set; }
        internal double LastRecordingCloseMs { get; private set; }
        internal double LastDesktopRestoreMs { get; private set; }
        internal double LastTotalPreciseMs { get; private set; }
        internal bool LastTimingCompleted { get; private set; }
        internal bool LastTimingSucceeded { get; private set; }

        internal string DiagnosticText { get => diagnostic.ToString(); }

        internal NativeCaptureOwner(IApplication app, IViewport viewport, IRenderService service,
            IWindow3D win, ICamera cam, int width, int height, bool useStereoViewportRecording = true)
        {
            this.app = app ?? throw new ArgumentNullException(nameof(app));
            this.viewport = viewport ?? throw new ArgumentNullException(nameof(viewport));
            this.service = service ?? throw new ArgumentNullException(nameof(service));
            this.win = win ?? throw new ArgumentNullException(nameof(win));
            this.cam = cam ?? throw new ArgumentNullException(nameof(cam));
            if (width < 1 || width > MaxDimension)
                throw new ArgumentOutOfRangeException(nameof(width), "Capture width must be 1..2048.");
            if (height < 1 || height > MaxDimension)
                throw new ArgumentOutOfRangeException(nameof(height), "Capture height must be 1..2048.");
            this.width = width;
            this.height = height;
            this.payload = checked(width * height * BytesPerPixel);
            this.useStereoViewportRecording = useStereoViewportRecording;

            RefreshDesktopSnapshot();

            Log("snapshot: window=" + origWidth + "x" + origHeight
                + " fovY=" + Fmt(origFovY) + " aspect=" + Fmt(origAspect)
                + " gaze=" + Fmt(origGaze) + " ortho=" + origOrtho
                + " rendering=" + origRenderingEnabled
                + " selected=" + width + "x" + height + " payload=" + payload
                + " stereoViewportRecording=" + useStereoViewportRecording);
        }

        // Reads the FULL live desktop state (window size, raw view matrix + 16 entries,
        // FOV/aspect/gaze/ortho, renderer flag) into locals via getters ONLY — no SDK
        // write happens here. Every value is validated first; orig* is committed atomically
        // only when all reads succeeded. Any failure throws before a single field is
        // refreshed, so the previous valid restore target stays intact.
        private void RefreshDesktopSnapshot()
        {
            var errors = new List<string>();
            int snapWidth = -1;
            int snapHeight = -1;
            Matrix snapView = default(Matrix);
            bool haveView = false;
            double[] snapRaw = null;
            double snapFovY = double.NaN;
            double snapAspect = double.NaN;
            double snapGaze = double.NaN;
            bool snapOrtho = false;
            bool snapRendering = false;
            try { snapWidth = win.Width; } catch (Exception ex) { errors.Add("snapshot width: " + ex.Message); }
            try { snapHeight = win.Height; } catch (Exception ex) { errors.Add("snapshot height: " + ex.Message); }
            try { snapView = cam.ViewMatrix; haveView = true; } catch (Exception ex) { errors.Add("snapshot view: " + ex.Message); }
            if (haveView)
            {
                try { snapRaw = ReadMatrix(snapView); }
                catch (Exception ex) { errors.Add("snapshot view entries: " + ex.Message); }
            }
            else errors.Add("Snapshot view matrix getter failed; no valid view captured.");
            try { snapFovY = cam.FieldOfViewY; } catch (Exception ex) { errors.Add("snapshot fovY: " + ex.Message); }
            try { snapAspect = cam.AspectRatio; } catch (Exception ex) { errors.Add("snapshot aspect: " + ex.Message); }
            try { snapGaze = cam.GazeLength; } catch (Exception ex) { errors.Add("snapshot gaze: " + ex.Message); }
            try { snapOrtho = cam.IsOrthographic; } catch (Exception ex) { errors.Add("snapshot ortho: " + ex.Message); }
            try { snapRendering = service.IsRenderingEnabled; } catch (Exception ex) { errors.Add("snapshot renderer: " + ex.Message); }

            if (snapWidth < 1 || snapHeight < 1)
                errors.Add("Snapshot window dimensions must be > 0, got " + snapWidth + "x" + snapHeight + ".");
            if (snapRaw != null)
            {
                for (int i = 0; i < 16; i++)
                {
                    if (!Finite(snapRaw[i]))
                        errors.Add("Snapshot view matrix entry [" + i + "] is not finite.");
                }
            }
            if (!Finite(snapFovY) || snapFovY < 0 || snapFovY > 180)
                errors.Add("Snapshot fovY must be finite and within 0..180, got " + Fmt(snapFovY) + ".");
            if (!Finite(snapAspect) || snapAspect <= 0)
                errors.Add("Snapshot aspect must be finite and positive, got " + Fmt(snapAspect) + ".");
            if (!Finite(snapGaze))
                errors.Add("Snapshot gaze length is not finite: " + Fmt(snapGaze) + ".");

            if (errors.Count > 0)
            {
                string message = "Snapshot refresh failed — restore target NOT updated: " + string.Join(" | ", errors);
                Log(message);
                throw new InvalidOperationException(message);
            }

            origWidth = snapWidth;
            origHeight = snapHeight;
            origViewMatrix = snapView;
            origViewRaw = snapRaw;
            origFovY = snapFovY;
            origAspect = snapAspect;
            origGaze = snapGaze;
            origOrtho = snapOrtho;
            origRenderingEnabled = snapRendering;
        }

        // Finite size validation only: the full desktop snapshot (surface + camera) and the
        // renderer flag are restored before returning, so the user viewport keeps rendering
        // between captures. Lifecycle flags stay untouched so Capture can run later.
        internal void Begin()
        {
            EnsureNotDisposed();
            if (restored)
                throw new InvalidOperationException(
                    "This NativeCaptureOwner was already restored; create a new instance instead of reusing it.");
            if (begun)
                return;
            var errors = new List<string>();
            mutated = true;
            // Mark pending restore BEFORE the first SDK write; cleared only by a clean
            // restore pass below.
            desktopRestoreNeeded = true;
            try
            {
                service.IsRenderingEnabled = false;
                if (!app.SurfaceSetSize(win, (uint)width, (uint)height))
                    errors.Add("begin: app.SurfaceSetSize returned false");
                else if (win.Width != width || win.Height != height)
                    errors.Add("begin: size selected=" + width + "x" + height
                        + " actual=" + win.Width + "x" + win.Height);
            }
            catch (Exception ex)
            {
                errors.Add("begin: " + ex.Message);
            }
            RestoreDesktopSnapshotAndRenderer(errors);
            if (errors.Count > 0)
            {
                string message = "NativeCaptureOwner begin failed: " + string.Join(" | ", errors);
                Log(message);
                throw new InvalidOperationException(message);
            }
            begun = true;
            Log("begin ok: surface " + width + "x" + height + " verified; desktop snapshot and renderer restored");
        }

        // Same pose legitimately yields identical pixels; buffer content alone does not prove
        // pose/image binding, so every readback below is pose/size verification, not hashing.
        // The reentrancy guard is owned exclusively by this wrapper: captureInProgress is
        // set BEFORE CaptureCore runs, so it also covers the live-snapshot getter reads —
        // a simulated SDK message pump that reenters Capture from get_ViewMatrix during
        // RefreshDesktopSnapshot is rejected here. The finally clears the guard on every
        // exit path of Core (success, input rejection, snapshot failure, cleanup failure).
        internal byte[] Capture(Matrix rawWorldToCamera, double fovY)
        {
            EnsureNotDisposed();
            if (captureInProgress)
                throw new InvalidOperationException(
                    "Reentrant Capture rejected — a capture is already in progress on this owner.");
            captureInProgress = true;
            try
            {
                return CaptureCore(rawWorldToCamera, fovY);
            }
            finally
            {
                captureInProgress = false;
            }
        }

        // Never touches captureInProgress — the wrapper owns it.
        private byte[] CaptureCore(Matrix rawWorldToCamera, double fovY)
        {
            if (!begun)
                throw new InvalidOperationException("Begin must succeed before Capture.");
            if (stickyNativeCloseFailed || stickyViewportCloseFailed || stickyDesktopRestoreFailed)
                throw new InvalidOperationException(
                    "A previous recording close or desktop restore failed; capture rejected until state is restored.");

            double[] expectedRaw = ReadMatrix(rawWorldToCamera);
            for (int i = 0; i < 16; i++)
            {
                if (!Finite(expectedRaw[i]))
                    throw new ArgumentOutOfRangeException(nameof(rawWorldToCamera),
                        "Expected view matrix entry [" + i + "] is not finite.");
            }
            if (double.IsInfinity(fovY))
                throw new ArgumentOutOfRangeException(nameof(fovY), "Infinite FOV is rejected.");
            bool fovApplied = !double.IsNaN(fovY);
            if (fovApplied && (!Finite(fovY) || fovY <= 0 || fovY >= 180))
                throw new ArgumentOutOfRangeException(nameof(fovY), "FOV must be finite and within 0..180.");

            // Commit the LIVE desktop snapshot as the restore target BEFORE any render/SDK
            // write. A getter or validation failure throws here: no SDK is mutated,
            // desktopRestoreNeeded is untouched, and no stale-snapshot restore is attempted
            // on that path. Runs under the wrapper's captureInProgress guard.
            RefreshDesktopSnapshot();

            // Fresh timing slate: after snapshot validation, before any SDK mutation.
            LastGateMs = double.NaN;
            LastResizeMs = double.NaN;
            LastCameraMs = double.NaN;
            LastRecordingEnableMs = double.NaN;
            LastBufferReadbackMs = double.NaN;
            LastPrivateCopyMs = double.NaN;
            LastRecordingCloseMs = double.NaN;
            LastDesktopRestoreMs = double.NaN;
            LastTotalPreciseMs = double.NaN;
            LastTimingCompleted = false;
            LastTimingSucceeded = false;

            var sw = Stopwatch.StartNew();
            var seg = Stopwatch.StartNew();
            bool succeeded = false;
            try
            {
                // Mark our mutation as pending restore BEFORE the first SDK write.
                desktopRestoreNeeded = true;
                // Renderer is gated only for the duration of this capture; Capture's cleanup
                // restores the freshest committed desktop snapshot and re-enables rendering.
                service.IsRenderingEnabled = false;
                LastGateMs = seg.Elapsed.TotalMilliseconds;
                seg.Restart();
                // Recording close after the previous Capture resets the window to desktop size,
                // so the selected native surface must be re-established before camera writes.
                if (!app.SurfaceSetSize(win, (uint)width, (uint)height))
                    Fail("capture", "app.SurfaceSetSize", "true", "false");
                if (win.Width != width || win.Height != height)
                    Fail("capture", "size", width + "x" + height, win.Width + "x" + win.Height);
                LastResizeMs = seg.Elapsed.TotalMilliseconds;
                seg.Restart();
                cam.IsOrthographic = false;
                cam.AspectRatio = width / (double)height;
                cam.ViewMatrix = rawWorldToCamera;
                if (fovApplied)
                    cam.FieldOfViewY = fovY;
                cam.Update();
                if (cam.IsOrthographic)
                    Fail("capture", "orthographic", "false", "true");
                VerifyPose(rawWorldToCamera, fovApplied ? (double?)fovY : null, "pre-readback");
                LastCameraMs = seg.Elapsed.TotalMilliseconds;
                seg.Restart();

                nativeRecordingPending = true;
                int ret = app.SetRecordingMode(win, true, (uint)width, (uint)height);
                if (ret < 0)
                    Fail("record", "app.SetRecordingMode", ">=0", "ret=" + ret);
                viewportRecordingPending = true;
                viewport.SetRecordingMode(true);
                LastRecordingEnableMs = seg.Elapsed.TotalMilliseconds;
                seg.Restart();

                byte[] buffer = app.SurfaceToContinuousBuffer((uint)width, (uint)height);
                LastBufferReadbackMs = seg.Elapsed.TotalMilliseconds;
                seg.Restart();
                if (buffer == null || buffer.Length < payload)
                    Fail("buffer", "length", ">= " + payload,
                        buffer == null ? "null" : buffer.Length.ToString());
                byte[] frame = new byte[payload];
                Buffer.BlockCopy(buffer, 0, frame, 0, payload);
                buffer = null;

                VerifyPose(rawWorldToCamera, fovApplied ? (double?)fovY : null, "post-copy");

                lastRecordingReadback();
                LastPrivateCopyMs = seg.Elapsed.TotalMilliseconds;
                succeeded = true;
                return frame;
            }
            finally
            {
                seg.Restart();
                bool cleanupFailed = false;
                    if (nativeRecordingPending)
                    {
                        try
                        {
                            int closeRet = app.SetRecordingMode(win, false, 0u, 0u);
                            if (closeRet < 0)
                            {
                                stickyNativeCloseFailed = true;
                                cleanupFailed = true;
                                Log("cleanup native close returned " + closeRet + " (sticky)");
                            }
                            else
                            {
                                nativeRecordingPending = false;
                            }
                        }
                        catch (Exception ex)
                        {
                            stickyNativeCloseFailed = true;
                            cleanupFailed = true;
                            Log("cleanup native close failed (sticky): " + ex.Message);
                        }
                    }
                    if (viewportRecordingPending)
                    {
                        try
                        {
                            viewport.SetRecordingMode(false);
                            viewportRecordingPending = false;
                        }
                        catch (Exception ex)
                        {
                            stickyViewportCloseFailed = true;
                            cleanupFailed = true;
                            Log("cleanup viewport close failed (sticky): " + ex.Message);
                        }
                    }
                    LastRecordingCloseMs = seg.Elapsed.TotalMilliseconds;
                    seg.Restart();
                    // Restore the freshest committed desktop snapshot (refreshed at this
                    // Capture's entry — NOT the construction-time entry) and the renderer,
                    // each field independently with readback; failures reject the frame and
                    // leave the lifecycle intact for Exit retry. Runs under the wrapper's
                    // captureInProgress guard so an SDK message pump cannot re-enter Capture
                    // mid-restore.
                    var restoreErrors = new List<string>();
                    RestoreDesktopSnapshotAndRenderer(restoreErrors);
                    LastDesktopRestoreMs = seg.Elapsed.TotalMilliseconds;
                    LastTotalPreciseMs = sw.Elapsed.TotalMilliseconds;
                    LastCaptureMs = sw.ElapsedMilliseconds;
                    if (restoreErrors.Count > 0)
                    {
                        cleanupFailed = true;
                        stickyDesktopRestoreFailed = true;
                        Log("capture desktop restore failed (sticky): " + string.Join(" | ", restoreErrors));
                    }
                    LastTimingCompleted = true;
                    LastTimingSucceeded = succeeded && !cleanupFailed;
                    if (succeeded && !cleanupFailed)
                    {
                        CaptureCount++;
                        if (CaptureCount <= 10 || CaptureCount % 100 == 0)
                            Log("capture#" + CaptureCount + " ok: actual=" + LastActualWidth + "x" + LastActualHeight
                                + " bytes=" + LastPayloadBytes + " aspect=" + Fmt(cam.AspectRatio));
                    }
                if (cleanupFailed)
                    throw new InvalidOperationException("Native capture cleanup failed; frame rejected.");
            }
        }

        // Candidate experiment (NOT an FPS claim): captures a stereo pair within ONE recording
        // period — recording enable, surface resize and desktop restore each run ONCE for both
        // eyes, instead of the per-eye close/restore cost of two sequential Capture calls.
        // Same caller contract as Capture: SIM UI thread only, guarded by the same
        // captureInProgress flag before any snapshot getter runs.
        internal byte[][] CaptureStereo(Matrix rawLeft, double fovLeft, Matrix rawRight, double fovRight)
        {
            EnsureNotDisposed();
            if (captureInProgress)
                throw new InvalidOperationException(
                    "Reentrant CaptureStereo rejected — a capture is already in progress on this owner.");
            captureInProgress = true;
            try
            {
                return CaptureStereoCore(rawLeft, fovLeft, rawRight, fovRight);
            }
            finally
            {
                captureInProgress = false;
            }
        }

        // Never touches captureInProgress — the wrapper owns it. Deliberately does NOT call
        // Capture: that would re-open a second recording period with its own close/restore.
        private byte[][] CaptureStereoCore(Matrix rawLeft, double fovLeft, Matrix rawRight, double fovRight)
        {
            if (!begun)
                throw new InvalidOperationException("Begin must succeed before CaptureStereo.");
            if (restored)
                throw new InvalidOperationException(
                    "This NativeCaptureOwner was already restored; create a new instance instead of reusing it.");
            if (stickyNativeCloseFailed || stickyViewportCloseFailed || stickyDesktopRestoreFailed)
                throw new InvalidOperationException(
                    "A previous recording close or desktop restore failed; capture rejected until state is restored.");

            // Full input validation BEFORE any snapshot getter or SDK write.
            double[] leftRaw = ReadMatrix(rawLeft);
            for (int i = 0; i < 16; i++)
            {
                if (!Finite(leftRaw[i]))
                    throw new ArgumentOutOfRangeException(nameof(rawLeft),
                        "Left view matrix entry [" + i + "] is not finite.");
            }
            double[] rightRaw = ReadMatrix(rawRight);
            for (int i = 0; i < 16; i++)
            {
                if (!Finite(rightRaw[i]))
                    throw new ArgumentOutOfRangeException(nameof(rawRight),
                        "Right view matrix entry [" + i + "] is not finite.");
            }
            if (double.IsInfinity(fovLeft))
                throw new ArgumentOutOfRangeException(nameof(fovLeft), "Infinite FOV is rejected.");
            bool leftFovApplied = !double.IsNaN(fovLeft);
            if (leftFovApplied && (!Finite(fovLeft) || fovLeft <= 0 || fovLeft >= 180))
                throw new ArgumentOutOfRangeException(nameof(fovLeft), "FOV must be finite and within 0..180.");
            if (double.IsInfinity(fovRight))
                throw new ArgumentOutOfRangeException(nameof(fovRight), "Infinite FOV is rejected.");
            bool rightFovApplied = !double.IsNaN(fovRight);
            if (rightFovApplied && (!Finite(fovRight) || fovRight <= 0 || fovRight >= 180))
                throw new ArgumentOutOfRangeException(nameof(fovRight), "FOV must be finite and within 0..180.");

            // ONE live desktop snapshot refresh for the whole pair, after complete input
            // validation and before any SDK write; on failure nothing is mutated and
            // desktopRestoreNeeded is untouched.
            RefreshDesktopSnapshot();

            // Fresh close-timing slate: after snapshot validation, before any SDK mutation.
            LastStereoNativeCloseMs = double.NaN;
            LastStereoViewportCloseMs = double.NaN;

            // Started after the snapshot so LastStereoPairMs covers paired capture + cleanup.
            var sw = Stopwatch.StartNew();
            bool succeeded = false;
            try
            {
                desktopRestoreNeeded = true;
                service.IsRenderingEnabled = false;
                if (!app.SurfaceSetSize(win, (uint)width, (uint)height))
                    Fail("stereo", "app.SurfaceSetSize", "true", "false");
                if (win.Width != width || win.Height != height)
                    Fail("stereo", "size", width + "x" + height, win.Width + "x" + win.Height);

                // LEFT eye — the only recording enable and surface resize of the pair.
                cam.IsOrthographic = false;
                cam.AspectRatio = width / (double)height;
                cam.ViewMatrix = rawLeft;
                if (leftFovApplied)
                    cam.FieldOfViewY = fovLeft;
                cam.Update();
                if (cam.IsOrthographic)
                    Fail("stereo-left", "orthographic", "false", "true");
                VerifyPose(rawLeft, leftFovApplied ? (double?)fovLeft : null, "stereo-left pre-readback");

                nativeRecordingPending = true;
                int ret = app.SetRecordingMode(win, true, (uint)width, (uint)height);
                if (ret < 0)
                    Fail("stereo record", "app.SetRecordingMode", ">=0", "ret=" + ret);
                if (useStereoViewportRecording)
                {
                    viewportRecordingPending = true;
                    viewport.SetRecordingMode(true);
                }

                byte[] buffer = app.SurfaceToContinuousBuffer((uint)width, (uint)height);
                if (buffer == null || buffer.Length < payload)
                    Fail("stereo-left buffer", "length", ">= " + payload,
                        buffer == null ? "null" : buffer.Length.ToString());
                byte[] leftFrame = new byte[payload];
                Buffer.BlockCopy(buffer, 0, leftFrame, 0, payload);
                buffer = null;

                VerifyPose(rawLeft, leftFovApplied ? (double?)fovLeft : null, "stereo-left post-copy");
                LastStereoLeftRaw = cam.ViewMatrix;
                LastStereoLeftFov = cam.FieldOfViewY;

                // RIGHT eye — same recording period: no second SetRecordingMode(true),
                // no second resize; only a camera pose swap and another readback.
                cam.ViewMatrix = rawRight;
                if (rightFovApplied)
                    cam.FieldOfViewY = fovRight;
                cam.Update();
                VerifyPose(rawRight, rightFovApplied ? (double?)fovRight : null, "stereo-right pre-readback");

                buffer = app.SurfaceToContinuousBuffer((uint)width, (uint)height);
                if (buffer == null || buffer.Length < payload)
                    Fail("stereo-right buffer", "length", ">= " + payload,
                        buffer == null ? "null" : buffer.Length.ToString());
                byte[] rightFrame = new byte[payload];
                Buffer.BlockCopy(buffer, 0, rightFrame, 0, payload);
                buffer = null;

                VerifyPose(rawRight, rightFovApplied ? (double?)fovRight : null, "stereo-right post-copy");
                LastStereoRightRaw = cam.ViewMatrix;
                LastStereoRightFov = cam.FieldOfViewY;
                // Verified readback while the right-eye camera state is still applied; the
                // desktop restore in finally overwrites cam fields afterwards.
                LastStereoAspect = cam.AspectRatio;

                // Verified source dimensions captured while recording is still open; the
                // pending close in finally may reset the window to desktop size.
                LastStereoWidth = win.Width;
                LastStereoHeight = win.Height;
                lastRecordingReadback();
                succeeded = true;
                return new byte[][] { leftFrame, rightFrame };
            }
            finally
            {
                bool cleanupFailed = false;
                if (nativeRecordingPending)
                {
                    var nativeCloseSw = Stopwatch.StartNew();
                    try
                    {
                        int closeRet = app.SetRecordingMode(win, false, 0u, 0u);
                        if (closeRet < 0)
                        {
                            stickyNativeCloseFailed = true;
                            cleanupFailed = true;
                            Log("stereo cleanup native close returned " + closeRet + " (sticky)");
                        }
                        else
                        {
                            nativeRecordingPending = false;
                        }
                    }
                    catch (Exception ex)
                    {
                        stickyNativeCloseFailed = true;
                        cleanupFailed = true;
                        Log("stereo cleanup native close failed (sticky): " + ex.Message);
                    }
                    finally
                    {
                        LastStereoNativeCloseMs = nativeCloseSw.Elapsed.TotalMilliseconds;
                    }
                }
                if (viewportRecordingPending)
                {
                    var viewportCloseSw = Stopwatch.StartNew();
                    try
                    {
                        viewport.SetRecordingMode(false);
                        viewportRecordingPending = false;
                    }
                    catch (Exception ex)
                    {
                        stickyViewportCloseFailed = true;
                        cleanupFailed = true;
                        Log("stereo cleanup viewport close failed (sticky): " + ex.Message);
                    }
                    finally
                    {
                        LastStereoViewportCloseMs = viewportCloseSw.Elapsed.TotalMilliseconds;
                    }
                }
                // Desktop restore + renderer run ONCE for the pair, regardless of close
                // outcome; failures reject the frame pair and stay retryable via Restore.
                var restoreErrors = new List<string>();
                RestoreDesktopSnapshotAndRenderer(restoreErrors);
                LastStereoPairMs = sw.Elapsed.TotalMilliseconds;
                if (restoreErrors.Count > 0)
                {
                    cleanupFailed = true;
                    stickyDesktopRestoreFailed = true;
                    Log("stereo capture desktop restore failed (sticky): " + string.Join(" | ", restoreErrors));
                }
                if (succeeded && !cleanupFailed)
                {
                    StereoPairCount++;
                    CaptureCount += 2;
                    // Actual-state reflects the verified RIGHT-eye values saved BEFORE the
                    // desktop restore above. Single-stage timings and LastCaptureMs are NOT
                    // touched here — they keep single-capture semantics; LastStereoPairMs is
                    // the paired duration.
                    LastActualWidth = LastStereoWidth;
                    LastActualHeight = LastStereoHeight;
                    LastPayloadBytes = payload;
                    LastActualAspectRatio = LastStereoAspect;
                    LastActualFieldOfViewY = LastStereoRightFov;
                    LastActualRawView = LastStereoRightRaw;
                    if (StereoPairCount <= 10 || StereoPairCount % 100 == 0)
                        Log("stereo-pair#" + StereoPairCount + " ok: source="
                            + LastStereoWidth + "x" + LastStereoHeight
                            + " bytes=" + payload + " pairMs=" + Fmt(LastStereoPairMs));
                }
                if (cleanupFailed)
                    throw new InvalidOperationException(
                        "Native stereo capture cleanup failed; frame pair rejected.");
            }
        }

        internal bool Restore()
        {
            if (captureInProgress)
                throw new InvalidOperationException(
                    "A capture is in progress; restore is rejected until the capture completes.");
            if (restored)
                return RestoreVerified;
            if (!mutated)
            {
                restored = true;
                RestoreVerified = true;
                Log("restore no-op: nothing mutated");
                return true;
            }
            var errors = new List<string>();
            try
            {
                if (nativeRecordingPending || viewportRecordingPending)
                {
                    // A recording close can reset the surface, so desktop write-back becomes
                    // needed again BEFORE we attempt the closes.
                    desktopRestoreNeeded = true;
                    if (nativeRecordingPending)
                    {
                        try
                        {
                            int closeRet = app.SetRecordingMode(win, false, 0u, 0u);
                            if (closeRet < 0)
                            {
                                stickyNativeCloseFailed = true;
                                errors.Add("native close ret=" + closeRet);
                            }
                            else
                            {
                                nativeRecordingPending = false;
                            }
                        }
                        catch (Exception ex)
                        {
                            stickyNativeCloseFailed = true;
                            errors.Add("native close: " + ex.Message);
                        }
                    }
                    if (viewportRecordingPending)
                    {
                        try
                        {
                            viewport.SetRecordingMode(false);
                            viewportRecordingPending = false;
                        }
                        catch (Exception ex)
                        {
                            stickyViewportCloseFailed = true;
                            errors.Add("viewport close: " + ex.Message);
                        }
                    }
                }
                // A clean owner exit (no pending close, no outstanding mutation of ours)
                // must NOT re-apply the stored snapshot: the user may have legitimately
                // moved the camera since the last capture.
                if (desktopRestoreNeeded)
                    RestoreDesktopSnapshotAndRenderer(errors);
            }
            finally
            {
                begun = false;
            }
            RestoreVerified = errors.Count == 0 && !nativeRecordingPending
                && !viewportRecordingPending && !desktopRestoreNeeded;
            restored = RestoreVerified;
            if (RestoreVerified)
                stickyDesktopRestoreFailed = false;
            Log("restore " + (RestoreVerified ? "verified" : "NOT verified")
                + (errors.Count == 0 ? "" : "; errors: " + string.Join(" | ", errors)));
            if (!RestoreVerified)
                throw new InvalidOperationException("NativeCaptureOwner restore failed: " + string.Join(" | ", errors));
            return true;
        }

        public void Dispose()
        {
            if (disposed)
                return;
            if (captureInProgress)
                throw new InvalidOperationException(
                    "A capture is in progress; dispose is rejected until the capture completes.");
            Exception restoreFailure = null;
            try
            {
                Restore();
            }
            catch (Exception ex)
            {
                restoreFailure = ex;
            }
            if (RestoreVerified)
            {
                disposed = true;
                Log("dispose; restoreVerified=true");
            }
            else
            {
                Log("dispose declined: restore not verified; instance kept for retry");
            }
            if (restoreFailure != null)
                throw new InvalidOperationException("Dispose: native capture state restore failed.", restoreFailure);
        }

        private void VerifyPose(Matrix expectedView, double? expectedFov, string stage)
        {
            if (win.Width != width || win.Height != height)
                Fail(stage, "size", width + "x" + height, win.Width + "x" + win.Height);
            double wantAspect = width / (double)height;
            double actualAspect = cam.AspectRatio;
            if (!Finite(actualAspect) || Math.Abs(actualAspect - wantAspect) > Tolerance)
                Fail(stage, "aspect", Fmt(wantAspect), Fmt(actualAspect));
            if (expectedFov.HasValue)
            {
                double actualFov = cam.FieldOfViewY;
                if (!Finite(actualFov) || Math.Abs(actualFov - expectedFov.Value) > Tolerance)
                    Fail(stage, "fovY", Fmt(expectedFov.Value), Fmt(actualFov));
            }
            double[] wanted = ReadMatrix(expectedView);
            double[] actual = ReadMatrix(cam.ViewMatrix);
            for (int i = 0; i < 16; i++)
            {
                if (!Finite(actual[i]) || Math.Abs(actual[i] - wanted[i]) > Tolerance)
                    Fail(stage, "view[" + i + "]", Fmt(wanted[i]), Fmt(actual[i]));
            }
        }

        // Restores the freshest committed desktop snapshot (constructor initial, then each
        // Capture's live refresh) — surface size and camera fields are applied and read back
        // independently, failures are collected and never thrown. The renderer flag and its
        // readback run in finally so the viewport always returns to rendering.
        private void RestoreDesktopSnapshotAndRenderer(List<string> errors)
        {
            int errorsBefore = errors.Count;
            try
            {
                try
                {
                    if (!app.SurfaceSetSize(win, (uint)origWidth, (uint)origHeight))
                        errors.Add("restore: app.SurfaceSetSize returned false");
                }
                catch (Exception ex) { errors.Add("restore size: " + ex.Message); }
                try
                {
                    cam.GazeLength = origGaze;
                }
                catch (Exception ex) { errors.Add("restore gaze: " + ex.Message); }
                try
                {
                    cam.ViewMatrix = origViewMatrix;
                }
                catch (Exception ex) { errors.Add("restore view: " + ex.Message); }
                try
                {
                    cam.FieldOfViewY = origFovY;
                }
                catch (Exception ex) { errors.Add("restore fov: " + ex.Message); }
                try
                {
                    cam.AspectRatio = origAspect;
                }
                catch (Exception ex) { errors.Add("restore aspect: " + ex.Message); }
                try
                {
                    cam.IsOrthographic = origOrtho;
                }
                catch (Exception ex) { errors.Add("restore ortho: " + ex.Message); }
                try
                {
                    cam.Update();
                }
                catch (Exception ex) { errors.Add("restore camera update: " + ex.Message); }
                VerifyRestore(errors);
            }
            finally
            {
                try
                {
                    service.IsRenderingEnabled = origRenderingEnabled;
                }
                catch (Exception ex) { errors.Add("restore renderer: " + ex.Message); }
                try
                {
                    if (service.IsRenderingEnabled != origRenderingEnabled)
                        errors.Add("renderer actual=" + service.IsRenderingEnabled
                            + " orig=" + origRenderingEnabled);
                }
                catch (Exception ex) { errors.Add("renderer readback: " + ex.Message); }
            }
            // Historical mutated=true does NOT mean a write-back is still required: only
            // errors NEW to this pass keep desktopRestoreNeeded set.
            if (errors.Count == errorsBefore)
                desktopRestoreNeeded = false;
        }

        private void VerifyRestore(List<string> errors)
        {
            try
            {
                if (win.Width != origWidth || win.Height != origHeight)
                    errors.Add("size actual=" + win.Width + "x" + win.Height
                        + " orig=" + origWidth + "x" + origHeight);
            }
            catch (Exception ex) { errors.Add("size readback: " + ex.Message); }
            try
            {
                double fov = cam.FieldOfViewY;
                if (!Finite(fov) || Math.Abs(fov - origFovY) > Tolerance)
                    errors.Add("fovY actual=" + Fmt(fov) + " orig=" + Fmt(origFovY));
            }
            catch (Exception ex) { errors.Add("fovY readback: " + ex.Message); }
            try
            {
                double aspect = cam.AspectRatio;
                if (!Finite(aspect) || Math.Abs(aspect - origAspect) > Tolerance)
                    errors.Add("aspect actual=" + Fmt(aspect) + " orig=" + Fmt(origAspect));
            }
            catch (Exception ex) { errors.Add("aspect readback: " + ex.Message); }
            try
            {
                double gaze = cam.GazeLength;
                if (!Finite(gaze) || Math.Abs(gaze - origGaze) > Tolerance)
                    errors.Add("gaze actual=" + Fmt(gaze) + " orig=" + Fmt(origGaze));
            }
            catch (Exception ex) { errors.Add("gaze readback: " + ex.Message); }
            try
            {
                if (cam.IsOrthographic != origOrtho)
                    errors.Add("ortho actual=" + cam.IsOrthographic + " orig=" + origOrtho);
            }
            catch (Exception ex) { errors.Add("ortho readback: " + ex.Message); }
            try
            {
                double[] actual = ReadMatrix(cam.ViewMatrix);
                for (int i = 0; i < 16; i++)
                {
                    if (!Finite(actual[i]) || Math.Abs(actual[i] - origViewRaw[i]) > Tolerance)
                        errors.Add("view[" + i + "] actual=" + Fmt(actual[i]) + " orig=" + Fmt(origViewRaw[i]));
                }
            }
            catch (Exception ex) { errors.Add("view readback: " + ex.Message); }
        }

        // Captures the verified pre-close source state; the recording close in cleanup may
        // reset the window to desktop size, so this must run before cleanup.
        private void lastRecordingReadback()
        {
            LastActualWidth = win.Width;
            LastActualHeight = win.Height;
            LastPayloadBytes = payload;
            LastActualAspectRatio = cam.AspectRatio;
            LastActualFieldOfViewY = cam.FieldOfViewY;
            LastActualRawView = cam.ViewMatrix;
        }

        private void Fail(string stage, string field, string selected, string actual)
        {
            string message = "verify failed at " + stage + ": " + field
                + " selected=" + selected + " actual=" + actual;
            Log(message);
            throw new InvalidOperationException(message);
        }

        private void EnsureNotDisposed()
        {
            if (disposed)
                throw new ObjectDisposedException(nameof(NativeCaptureOwner));
        }

        private void Log(string message)
        {
            if (diagnostic.Length >= MaxDiagnosticChars)
                return;
            diagnostic.Append("[native-capture] ").Append(message).Append('\n');
            if (diagnostic.Length > MaxDiagnosticChars)
                diagnostic.Length = MaxDiagnosticChars;
        }

        private static bool Finite(double v)
        {
            return !double.IsNaN(v) && !double.IsInfinity(v);
        }

        private static double[] ReadMatrix(Matrix m)
        {
            var v = new double[16];
            for (int i = 0; i < 16; i++)
                v[i] = m[i];
            return v;
        }

        private static string Fmt(double v)
        {
            return v.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
        }
    }
}
