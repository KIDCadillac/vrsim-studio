// StereoResampleWorker.cs — H2 candidate: move per-eye XR reprojection OFF the SIM UI
// thread. Pure CPU, zero SDK surface (no IApplication/ICamera/Matrix, no native, no IO,
// no dispatcher). NOT wired into VCFrameProducer yet — offline QA + Codex review first.
using System;
using System.Diagnostics;
using System.Threading;

namespace VRSIM
{
    /// <summary>Immutable single-frame request. Both eye payloads are deep-cloned at
    /// construction so caller mutation of the source buffers cannot alter what is
    /// resampled (explicit offline-QA check).</summary>
    internal sealed class StereoResampleJob
    {
        private readonly int _generation;
        private readonly ulong _frameSerial;
        private readonly ulong _poseSerial;
        private readonly byte[] _left;
        private readonly byte[] _right;
        private readonly VCFrameProducer.EyeProjectionPlan _leftPlan;
        private readonly VCFrameProducer.EyeProjectionPlan _rightPlan;

        internal StereoResampleJob(
            int generation,
            ulong frameSerial,
            ulong poseSerial,
            byte[] leftBgra,
            byte[] rightBgra,
            VCFrameProducer.EyeProjectionPlan leftPlan,
            VCFrameProducer.EyeProjectionPlan rightPlan)
        {
            if (frameSerial == 0UL)
                throw new ArgumentOutOfRangeException("frameSerial", "must be > 0");
            if (poseSerial == 0UL)
                throw new ArgumentOutOfRangeException("poseSerial", "must be > 0");
            if (leftBgra == null || leftBgra.Length != CaptureProfile.PayloadBytes)
                throw new ArgumentException("left must be exactly " + CaptureProfile.PayloadBytes + " bytes (BGRA)", "leftBgra");
            if (rightBgra == null || rightBgra.Length != CaptureProfile.PayloadBytes)
                throw new ArgumentException("right must be exactly " + CaptureProfile.PayloadBytes + " bytes (BGRA)", "rightBgra");
            if (!leftPlan.Valid)
                throw new ArgumentException("left plan invalid: " + leftPlan.RejectReason, "leftPlan");
            if (!rightPlan.Valid)
                throw new ArgumentException("right plan invalid: " + rightPlan.RejectReason, "rightPlan");

            _generation = generation;
            _frameSerial = frameSerial;
            _poseSerial = poseSerial;
            _left = (byte[])leftBgra.Clone();
            _right = (byte[])rightBgra.Clone();
            _leftPlan = leftPlan;
            _rightPlan = rightPlan;
        }

        internal int Generation { get { return _generation; } }
        internal ulong FrameSerial { get { return _frameSerial; } }
        internal ulong PoseSerial { get { return _poseSerial; } }
        internal byte[] LeftBgra { get { return _left; } }
        internal byte[] RightBgra { get { return _right; } }
        internal VCFrameProducer.EyeProjectionPlan LeftPlan { get { return _leftPlan; } }
        internal VCFrameProducer.EyeProjectionPlan RightPlan { get { return _rightPlan; } }
    }

    /// <summary>Completed (or failed) outcome of one job. Left/Right are null when
    /// Error != null; per-eye milliseconds are Stopwatch-based, NaN if that eye did not
    /// complete. Carries no SDK object references.</summary>
    internal sealed class StereoResampleResult
    {
        internal readonly int Generation;
        internal readonly ulong FrameSerial;
        internal readonly ulong PoseSerial;
        internal readonly byte[] LeftBgra;
        internal readonly byte[] RightBgra;
        internal readonly double LeftResampleMs;
        internal readonly double RightResampleMs;
        internal readonly Exception Error;

        internal StereoResampleResult(
            int generation,
            ulong frameSerial,
            ulong poseSerial,
            byte[] leftBgra,
            byte[] rightBgra,
            double leftResampleMs,
            double rightResampleMs,
            Exception error)
        {
            Generation = generation;
            FrameSerial = frameSerial;
            PoseSerial = poseSerial;
            LeftBgra = leftBgra;
            RightBgra = rightBgra;
            LeftResampleMs = leftResampleMs;
            RightResampleMs = rightResampleMs;
            Error = error;
        }
    }

    /// <summary>One dedicated background thread. Bounded state: at most 1 in-flight,
    /// 1 pending (latest-wins), 1 completed (latest-wins). No Task/thread-pool/
    /// BlockingCollection, no unbounded queue, no dispatcher callback. All state under a
    /// single lock; the resample computation NEVER holds the lock.</summary>
    internal sealed class StereoResampleWorker : IDisposable
    {
        private readonly int _generation;
        private readonly Func<byte[], VCFrameProducer.EyeProjectionPlan, byte[]> _resample;
        private readonly Thread _worker;
        private readonly object _sync = new object();

        private StereoResampleJob _pending;         // 0..1
        private StereoResampleJob _inFlight;        // 0..1, worker-owned while computed
        private StereoResampleResult _completed;    // 0..1
        private bool _stopped;
        private ulong _lastAcceptedFrameSerial;
        private int _droppedPending, _droppedCompleted, _rejectedJobs;

        /// <summary>resample defaults to the production pure-static XR reprojection;
        /// tests inject a same-signature delegate to block or throw.</summary>
        internal StereoResampleWorker(
            int generation,
            Func<byte[], VCFrameProducer.EyeProjectionPlan, byte[]> resample = null)
        {
            _generation = generation;
            _resample = resample ??
                new Func<byte[], VCFrameProducer.EyeProjectionPlan, byte[]>(VCFrameProducer.ResampleToXrFrustum);
            _worker = new Thread(WorkerLoop)
            {
                IsBackground = true,
                Name = "VRSIM-StereoResample-g" + generation
            };
            _worker.Start();
        }

        /// <summary>Accepts only a same-generation job with a strictly increasing
        /// FrameSerial. If the pending slot is full the new job REPLACES it (latest wins)
        /// and DroppedPending increments. Returns false when stopped / generation
        /// mismatch / serial not increasing / null.</summary>
        internal bool TrySubmit(StereoResampleJob job)
        {
            lock (_sync)
            {
                if (_stopped || job == null || job.Generation != _generation ||
                    job.FrameSerial <= _lastAcceptedFrameSerial)
                {
                    _rejectedJobs++;
                    return false;
                }
                _lastAcceptedFrameSerial = job.FrameSerial;
                if (_pending != null) _droppedPending++;
                _pending = job;
                Monitor.PulseAll(_sync);
                return true;
            }
        }

        /// <summary>UI-side poll: takes the freshest completed result, if any. After Stop
        /// this never yields a result.</summary>
        internal bool TryTakeCompleted(out StereoResampleResult result)
        {
            lock (_sync)
            {
                if (_stopped || _completed == null)
                {
                    result = null;
                    return false;
                }
                result = _completed;
                _completed = null;
                return true;
            }
        }

        /// <summary>Stops accepting work, clears pending/completed and suppresses every
        /// later result (a late in-flight completion is discarded, never republished).
        /// Joins OUTSIDE the lock with timeoutMs; returns true ONLY if the thread really
        /// exited — never Abort. Timeout → false; a repeated Stop retries the join.
        /// Called from the worker thread itself: rejected with false (no self-join).</summary>
        internal bool Stop(int timeoutMs)
        {
            if (timeoutMs < 0) throw new ArgumentOutOfRangeException("timeoutMs");
            Thread t = _worker;
            if (ReferenceEquals(t, Thread.CurrentThread)) return false;
            lock (_sync)
            {
                if (!_stopped)
                {
                    _stopped = true;
                    _pending = null;
                    _completed = null;
                    Monitor.PulseAll(_sync);
                }
            }
            if (!t.IsAlive) return true;
            return t.Join(timeoutMs);
        }

        /// <summary>Stop(500); a timeout THROWS TimeoutException — the worker thread is
        /// still alive and we never claim a completed release.</summary>
        public void Dispose()
        {
            if (!Stop(500))
                throw new TimeoutException(
                    "StereoResampleWorker generation=" + _generation +
                    " did not exit within 500 ms — not released");
        }

        // ---------------- bounded-state / counters (all under the same lock) ----------------
        internal int PendingCount { get { lock (_sync) return _pending != null ? 1 : 0; } }
        internal int InFlightCount { get { lock (_sync) return _inFlight != null ? 1 : 0; } }
        internal int CompletedCount { get { lock (_sync) return _completed != null ? 1 : 0; } }
        internal int DroppedPending { get { lock (_sync) return _droppedPending; } }
        internal int DroppedCompleted { get { lock (_sync) return _droppedCompleted; } }
        internal int RejectedJobs { get { lock (_sync) return _rejectedJobs; } }
        internal bool IsStopped { get { lock (_sync) return _stopped; } }

        /// <summary>Honest thread liveness — never inferred from flags alone.</summary>
        internal bool HasExited { get { return !_worker.IsAlive; } }

        private void WorkerLoop()
        {
            while (true)
            {
                StereoResampleJob job;
                lock (_sync)
                {
                    while (_pending == null && !_stopped) Monitor.Wait(_sync);
                    if (_stopped) return;
                    job = _pending;
                    _pending = null;
                    _inFlight = job;
                }

                StereoResampleResult result = Compute(job);   // NO lock held during resample

                lock (_sync)
                {
                    _inFlight = null;
                    if (_stopped) continue;                   // late finish after Stop → discard
                    if (_completed != null) _droppedCompleted++;
                    _completed = result;
                    Monitor.PulseAll(_sync);
                }
            }
        }

        private StereoResampleResult Compute(StereoResampleJob job)
        {
            byte[] left = null, right = null;
            double leftMs = double.NaN, rightMs = double.NaN;
            Exception error = null;
            try
            {
                Stopwatch sw = Stopwatch.StartNew();
                left = _resample(job.LeftBgra, job.LeftPlan);
                leftMs = sw.Elapsed.TotalMilliseconds;
                sw = Stopwatch.StartNew();
                right = _resample(job.RightBgra, job.RightPlan);
                rightMs = sw.Elapsed.TotalMilliseconds;
                if (left == null || left.Length != CaptureProfile.PayloadBytes ||
                    right == null || right.Length != CaptureProfile.PayloadBytes)
                {
                    error = new InvalidOperationException(
                        "resample delegate returned null or wrong-length payload (expected " +
                        CaptureProfile.PayloadBytes + " bytes)");
                    left = null;
                    right = null;
                }
            }
            catch (Exception ex)
            {
                // Worker exceptions become the result — the thread NEVER dies.
                error = ex;
                left = null;
                right = null;
            }
            return new StereoResampleResult(
                job.Generation, job.FrameSerial, job.PoseSerial, left, right, leftMs, rightMs, error);
        }
    }
}
