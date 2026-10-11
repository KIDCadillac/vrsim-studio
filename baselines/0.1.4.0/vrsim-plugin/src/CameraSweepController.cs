// CameraSweepController.cs — VRSIM-PRODUCT-02-LIVE-PIPELINE (0.1.1.8).
//
// MACHINE-DRIVEN MOTION PROOF. No HMD tracking, no wear detection, no user.
// A deterministic camera sweep (yaw = 15°·sin(2π·0.25Hz·t), 20 s) drives the REAL VC
// camera through the REAL renderer; every layer is instrumented with serials/hashes:
//   camera matrix → VC frame hash → ring commit/consume → D3D texture → submitted content.
// After a PASS, a RECORDED_XR_POSE_REPLAY (CARD15 real sample + local-frame deltas) is
// pushed through the same production bridge to prove the XR-pose → VC-camera bridge.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Threading;
using VisualComponents.Create3D;

namespace VRSIM
{
    internal sealed class CameraSweepController
    {
        internal static readonly int FrameW = VCFrameProducer.FrameW;
        internal static readonly int FrameH = VCFrameProducer.FrameH;
        private const double SweepSeconds = 20.0;
        private const double SweepHz = 0.25;
        private const double SweepAmpDeg = 15.0;

        private readonly VRSIMPlugin _host;
        private readonly Dispatcher _disp;
        private IApplication _app;
        private VisualComponents.Create3D.IWindow3D _win;
        private ICamera _cam;
        private Matrix _entry;
        private double[] _R_E = new double[9], _P_E = new double[3];
        private double _fovY = 35.0;
        private DispatcherTimer _timer;
        private readonly Stopwatch _clock = new Stopwatch();
        private string _ev;

        private ulong _contentSerial;
        private string _lastHash = "";
        private double _lastYaw = -999;
        private int _cameraChangeCount, _hashChangeCount, _duplicates, _stale;
        private double _freshP50, _freshP95;
        private readonly System.Collections.Generic.List<double> _fresh = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _intervals = new System.Collections.Generic.List<double>();
        private DateTime _lastTick = DateTime.MinValue;
        private readonly StringBuilder _sweepCsv = new StringBuilder("tMs,yawDeg,frameSerial,hash8,freshMs,submitRc\n");
        private readonly StringBuilder _texCsv = new StringBuilder("tMs,frameSerial,texHash8\n");
        private readonly System.Collections.Generic.List<byte[]> _keyframes = new System.Collections.Generic.List<byte[]>();
        private int _sweepStage; // 0 = running, 1 = done
        private bool _keyPlus, _keyMinus, _keyZero;
        private double _prevYaw = -999;
        private volatile bool _canceled;          // CancelAndRestore: stop ticks, restore, no host success report
        private double _fovEntry = 35.0;          // entry FOV/Gaze for full restore (UI-review card item 2)
        private double _gazeEntry = double.NaN;

        public bool SweepPass { get; private set; }
        public string Report { get; private set; } = "";

        public CameraSweepController(VRSIMPlugin host, Dispatcher disp)
        {
            _host = host;
            _disp = disp;
            _ev = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "KUKA", "iiQWorks.Sim", "VRSIM", "sweep");
            try { Directory.CreateDirectory(_ev); } catch { }
        }

        /// <summary>Entry point (UI thread). Runs sweep, then replay, writes all evidence.
        /// entryView is the RAW ViewMatrix (world-to-camera) from the Enter-time snapshot;
        /// gazeLength is that snapshot's Gaze (optional, NaN = unknown → skipped on restore).</summary>
        public void Run(Matrix entryView, double fovY, double gazeLength = double.NaN)
        {
            _disp.Invoke((System.Action)(() =>
            {
                try
                {
                    _app = _host.Application;
                    var vp = VisualComponents.UX.Input.HelperViewport.ActiveViewport;
                    _win = vp.Window;
                    _cam = _win.Camera;
                    _entry = entryView;
                    _fovY = fovY;
                    _fovEntry = fovY;
                    _gazeEntry = gazeLength;
                    VCFrameProducer.MatToRotPos(_entry, _R_E, _P_E);
                    _app.SetRecordingMode(_win, true, (uint)FrameW, (uint)FrameH);
                    _clock.Start();
                    _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
                    _timer.Tick += (s, e) => SweepTick();
                    _timer.Start();
                }
                catch (Exception ex)
                {
                    _host.Log("SWEEP_START_FAIL " + ex.Message);
                    Finish();
                }
            }));
        }

        private void SweepTick()
        {
            if (_canceled || _sweepStage != 0) return;
            try
            {
                double t = _clock.Elapsed.TotalSeconds;
                if (t >= SweepSeconds) { EndSweep(); return; }
                double yaw = SweepAmpDeg * Math.Sin(2 * Math.PI * SweepHz * t);
                DoFrame(t, yaw, keyframes: true);
                if (_lastYaw == -999 || Math.Abs(yaw - _lastYaw) > 1e-9) _cameraChangeCount++;
                _lastYaw = yaw;
            }
            catch (Exception ex)
            {
                _host.Log("SWEEP_TICK_FAIL " + ex.GetType().Name + ": " + ex.Message);
                EndSweep();
            }
        }

        private void DoFrame(double t, double yawDeg, bool keyframes)
        {
            // camera = entry rotated by LOCAL yaw about its own up axis (single DOF, PHASE 2)
            double[] R_test = Mul3(_R_E, QToMatrix(QAxis(0, 1, 0, yawDeg)));
            Matrix target = RotPosToMat(R_test, _P_E);
            _cam.ViewMatrix = target;

            var raw = PollFresh();
            _contentSerial++;
            int rc = _host.SubmitStereo(raw, raw, FrameW, FrameH, FrameW * 4, _contentSerial, 0, yawDeg);
            string h8 = Hash8(raw);
            if (h8 == _lastHash) _duplicates++; else _hashChangeCount++;
            _lastHash = h8;
            _sweepCsv.Append(((long)(t * 1000)).ToString(CultureInfo.InvariantCulture)).Append(',')
                     .Append(yawDeg.ToString("0.00", CultureInfo.InvariantCulture)).Append(',')
                     .Append(_contentSerial).Append(',').Append(h8).Append(',')
                     .Append(_fresh.Count > 0 ? _fresh[_fresh.Count - 1].ToString("0.0", CultureInfo.InvariantCulture) : "0")
                     .Append(',').Append(rc).Append('\n');

            // keyframes at yaw crossings (PHASE 5)
            if (keyframes)
            {
                // 0.1.2.1: sample spacing (~200ms at 10Hz content) can straddle the +-1 deg
                // zero window (only +-42ms wide) — detect the DESCENDING zero crossing instead
                if (!_keyZero && _prevYaw > 0.0 && yawDeg <= 0.0) { _keyZero = true; _keyframes.Add(raw); SavePng("sweep-yaw-0.png", raw); }
                _prevYaw = yawDeg;
                if (!_keyPlus && yawDeg > SweepAmpDeg * 0.92) { _keyPlus = true; _keyframes.Add(raw); SavePng("sweep-yaw-plus15.png", raw); }
                if (!_keyMinus && yawDeg < -SweepAmpDeg * 0.92) { _keyMinus = true; _keyframes.Add(raw); SavePng("sweep-yaw-minus15.png", raw); }
            }

            // PHASE 8: native texture QA readback every 30 content frames
            if (_contentSerial % 30 == 0)
            {
                byte[] tex = _host.DumpSceneTextureBytes(0);
                if (tex != null)
                {
                    _texCsv.Append(((long)(t * 1000)).ToString(CultureInfo.InvariantCulture)).Append(',')
                           .Append(_contentSerial).Append(',').Append(Hash8(tex)).Append('\n');
                    int n = _texCsv.ToString().Split('\n').Length;
                    if (n == 3) SavePng("native-frame-A.png", tex);
                    if (n == 8) SavePng("native-frame-B.png", tex);
                    if (n == 13) SavePng("native-frame-C.png", tex);
                }
            }
        }

        private byte[] PollFresh()
        {
            var sw = Stopwatch.StartNew();
            byte[] copy = null;
            var deadline = Environment.TickCount + 2000;
            while (Environment.TickCount < deadline)
            {
                var raw = _app.SurfaceToContinuousBuffer((uint)FrameW, (uint)FrameH);
                if (raw != null && raw.Length >= CaptureProfile.PayloadBytes)
                {
                    copy = new byte[CaptureProfile.PayloadBytes];
                    Buffer.BlockCopy(raw, 0, copy, 0, CaptureProfile.PayloadBytes);
                    VCFrameProducer.ForceAlpha(copy);
                    string h = Hash8(copy);
                    if (h != _lastHash) break; // fresh frame for the NEW camera pose (PHASE 16/17)
                }
                System.Threading.Thread.Sleep(6);
            }
            sw.Stop();
            _fresh.Add(sw.Elapsed.TotalMilliseconds);
            if (copy == null) { _stale++; copy = new byte[CaptureProfile.PayloadBytes]; }
            return copy;
        }

        private void EndSweep()
        {
            if (_sweepStage != 0) return;
            _sweepStage = 1;
            try { _timer.Stop(); } catch { }
            RestoreCamera();   // raw view + FOV + Gaze (no inversion — _entry is raw)
            _fresh.Sort();
            _freshP50 = _fresh.Count > 0 ? _fresh[_fresh.Count / 2] : 0;
            _freshP95 = _fresh.Count > 0 ? _fresh[(int)(_fresh.Count * 0.95)] : 0;
            File.WriteAllText(Path.Combine(_ev, "camera-sweep.csv"), _sweepCsv.ToString());
            File.WriteAllText(Path.Combine(_ev, "d3d-texture-hashes.csv"), _texCsv.ToString());

            // PHASE 20: pixel diffs between keyframes
            double d0p = _keyframes.Count > 2 ? DiffRatio(_keyframes[0], _keyframes[1]) : -1;
            double d0m = _keyframes.Count > 2 ? DiffRatio(_keyframes[0], _keyframes[2]) : -1;
            double dpm = _keyframes.Count > 2 ? DiffRatio(_keyframes[1], _keyframes[2]) : -1;

            var st = _host.ReadStereoStats();
            ulong consumedSerial = st.HasValue ? st.Value.lastConsumedSerial : 0;
            ulong consumedYawChanges = st.HasValue ? st.Value.consumedYawChanges : 0;
            double consumedYaw = st.HasValue ? st.Value.lastConsumedYawDeg : -9999;
            ulong mismatch = st.HasValue ? st.Value.poseImageMismatch : 0;
            ulong d3dUploads = st.HasValue ? st.Value.d3dUploadCount : 0;
            ulong beginFrameFailed = st.HasValue ? st.Value.beginFrameFailed : 0;

            // PHASE 11: dynamic gates
            bool camOk = _cameraChangeCount > 100;
            bool hashOk = _hashChangeCount > 100;
            bool ringCommitOk = _contentSerial > 100;
            bool ringConsumeOk = consumedSerial > 100;
            bool texOk = _texCsv.ToString().Split('\n').Length > 4; // ≥3 distinct readback rows
            bool submitOk = d3dUploads >= (ulong)(_contentSerial * 0.9); // 0.1.2.0: consume COUNT, not last-serial
            bool yawOk = consumedYawChanges > 100;
            bool keysOk = _keyframes.Count >= 3 && d0p > 0.01 && d0m > 0.01 && dpm > 0.01;
            SweepPass = camOk && hashOk && ringCommitOk && ringConsumeOk && texOk && submitOk && yawOk && keysOk;

            var ci = CultureInfo.InvariantCulture;
            string j = "{\"cameraSweepPass\":" + (SweepPass ? "true" : "false") +
                ",\"cameraChangeCount\":" + _cameraChangeCount +
                ",\"hashChangeCount\":" + _hashChangeCount +
                ",\"duplicates\":" + _duplicates + ",\"staleTimeouts\":" + _stale +
                ",\"contentSerial\":" + _contentSerial +
                ",\"ringConsumedSerial\":" + consumedSerial +
                ",\"consumedYawChanges\":" + consumedYawChanges +
                ",\"lastConsumedYawDeg\":" + consumedYaw.ToString("0.00", ci) +
                ",\"freshP50Ms\":" + _freshP50.ToString("0.0", ci) +
                ",\"freshP95Ms\":" + _freshP95.ToString("0.0", ci) +
                ",\"imageDiff0vsPlus15\":" + d0p.ToString("0.000000", ci) +
                ",\"imageDiff0vsMinus15\":" + d0m.ToString("0.000000", ci) +
                ",\"imageDiffPlusMinus\":" + dpm.ToString("0.000000", ci) +
                ",\"poseImageMismatch\":" + mismatch +
                ",\"gates\":{\"camera\":" + B(camOk) + ",\"hash\":" + B(hashOk) +
                ",\"ringCommit\":" + B(ringCommitOk) + ",\"ringConsume\":" + B(ringConsumeOk) +
                ",\"textureQa\":" + B(texOk) + ",\"submitKeptUp\":" + B(submitOk) +
                ",\"yawChannel\":" + B(yawOk) + ",\"keyframesDistinct\":" + B(keysOk) + "}}";
            File.WriteAllText(Path.Combine(_ev, "dynamic-pipeline-validation.json"), j);
            Report = j;
            _host.Log("SWEEP_DONE pass=" + SweepPass + " camChanges=" + _cameraChangeCount +
                " hashChanges=" + _hashChangeCount + " consumed=" + consumedSerial +
                " yawChanges=" + consumedYawChanges + " diff0+15=" + d0p.ToString("0.0000", ci));
            try { _app.SetRecordingMode(_win, false, 0, 0); } catch { }
            if (_canceled) { _host.Log("SWEEP_CANCELED no_host_callback"); return; }
            _host.SweepFinished(SweepPass, j);
        }

        // ---------------- PHASE 21-24: recorded XR pose replay through the production bridge ----
        /// <summary>CARD15 recorded real Quest left-eye pose (sample-run1.json, index 0).</summary>
        internal static readonly double[] RecordedPosM = { -0.034067925065755844, -0.0012358028907328844, -0.000067153974669 };
        internal static readonly double[] RecordedQuat = { 0.06400839239358902, 0.0000434752437286, 0.002507588127627969, 0.9979462027549744 };

        public void RunReplay()
        {
            if (_canceled) { _host.Log("REPLAY_SKIPPED_CANCELED"); return; }
            _disp.Invoke((System.Action)(() =>
            {
                try
                {
                    _app.SetRecordingMode(_win, true, (uint)FrameW, (uint)FrameH);
                    // production bridge: entry-relative, mapping-invariant deltas applied to the
                    // entry camera (identical formula to the live path, TrackingSelfTest-verified)
                    var probes = new (string name, double yawDeg, double pitchDeg)[]
                    {
                        ("replay-center", 0, 0),
                        ("replay-yaw-left", 10, 0),
                        ("replay-center2", 0, 0),
                        ("replay-yaw-right", -10, 0),
                        ("replay-pitch-up", 0, 10),
                        ("replay-center3", 0, 0),
                    };
                    var rows = new StringBuilder("pose,cameraYawDeg,frameSerial,hash8,screenX,screenY\n");
                    double refX0 = 0, refY0 = 0; bool first = true;
                    double[] yawProbe = null, pitchProbe = null;
                    foreach (var (name, yaw, pitch) in probes)
                    {
                        double[] R_cam = Mul3(_R_E, Mul3(QToMatrix(QAxis(0, 1, 0, yaw)), QToMatrix(QAxis(1, 0, 0, pitch))));
                        _cam.ViewMatrix = RotPosToMat(R_cam, _P_E);
                        byte[] img = PollFresh();
                        _contentSerial++;
                        _host.SubmitStereo(img, img, FrameW, FrameH, FrameW * 4, _contentSerial, 1, yaw);
                        var sp = Project(RefPoint(), R_cam);
                        if (first) { refX0 = sp.x; refY0 = sp.y; first = false; }
                        if (name == "replay-yaw-left") yawProbe = new[] { sp.x, sp.y };
                        if (name == "replay-pitch-up") pitchProbe = new[] { sp.x, sp.y };
                        rows.Append(name).Append(',').Append(yaw.ToString("0", CultureInfo.InvariantCulture))
                            .Append(',').Append(_contentSerial).Append(',').Append(Hash8(img))
                            .Append(',').Append(sp.x.ToString("0.0", CultureInfo.InvariantCulture))
                            .Append(',').Append(sp.y.ToString("0.0", CultureInfo.InvariantCulture)).Append('\n');
                        SavePng(name + ".png", img);
                    }
                    // oracle: yaw left 10° => fixed world point moves RIGHT (+x); pitch up => DOWN (+y)
                    bool yawOk = yawProbe != null && yawProbe[0] > refX0 + 2;
                    bool pitchOk = pitchProbe != null && pitchProbe[1] > refY0 + 2;
                    File.WriteAllText(Path.Combine(_ev, "recorded-xr-pose-replay.csv"), rows.ToString());
                    var st = _host.ReadStereoStats();
                    string j = "{\"replayPass\":" + (yawOk && pitchOk ? "true" : "false") +
                        ",\"yawOraclePass\":" + B(yawOk) + ",\"pitchOraclePass\":" + B(pitchOk) +
                        ",\"refScreenX0\":" + refX0.ToString("0.0", CultureInfo.InvariantCulture) +
                        ",\"refScreenY0\":" + refY0.ToString("0.0", CultureInfo.InvariantCulture) +
                        ",\"yawLeftScreenX\":" + (yawProbe != null ? yawProbe[0].ToString("0.0", CultureInfo.InvariantCulture) : "0") +
                        ",\"pitchUpScreenY\":" + (pitchProbe != null ? pitchProbe[1].ToString("0.0", CultureInfo.InvariantCulture) : "0") +
                        ",\"consumedSerial\":" + (st.HasValue ? st.Value.lastConsumedSerial : 0) +
                        ",\"poseSource\":\"CARD15 sample-run1.json index0 (real Quest)\"}";
                    File.WriteAllText(Path.Combine(_ev, "tracking-replay-validation.json"), j);
                    _host.Log("REPLAY_DONE yawOk=" + yawOk + " pitchOk=" + pitchOk);
                    try { _app.SetRecordingMode(_win, false, 0, 0); } catch { }
                    RestoreCamera();
                    if (_canceled) { _host.Log("REPLAY_CANCELED no_host_callback"); return; }
                    _host.ReplayFinished(yawOk && pitchOk, j);
                }
                catch (Exception ex)
                {
                    _host.Log("REPLAY_FAIL " + ex.GetType().Name + ": " + ex.Message);
                    try { _app.SetRecordingMode(_win, false, 0, 0); } catch { }
                    RestoreCamera();
                    if (!_canceled)
                        _host.ReplayFinished(false, "{\"replayPass\":false,\"error\":\"" + ex.Message + "\"}");
                }
            }));
        }

        private double[] _refPt;
        private double[] RefPoint()
        {
            if (_refPt != null) return _refPt;
            double[] fwd = { -_R_E[2], -_R_E[5], -_R_E[8] };
            double[] right = { _R_E[0], _R_E[3], _R_E[6] };
            _refPt = new double[3];
            for (int i = 0; i < 3; i++) _refPt[i] = _P_E[i] + fwd[i] * 2000.0 + right[i] * 300.0;
            return _refPt;
        }
        private (bool ok, double x, double y) Project(double[] worldPt, double[] R)
        {
            double[] d = Sub(worldPt, _P_E);
            double x = d[0] * R[0] + d[1] * R[3] + d[2] * R[6];
            double y = d[0] * R[1] + d[1] * R[4] + d[2] * R[7];
            double z = d[0] * R[2] + d[1] * R[5] + d[2] * R[8];
            if (z > -10) return (false, 0, 0);
            double tanY = Math.Tan(_fovY * Math.PI / 360.0);
            double tanX = tanY * FrameW / FrameH;
            return (true, ((x / -z) / tanX + 1) * 0.5 * FrameW, (1 - ((y / -z) / tanY + 1) * 0.5) * FrameH);
        }

        private static string B(bool v) { return v ? "true" : "false"; }
        private static double DiffRatio(byte[] a, byte[] b)
        {
            if (a == null || b == null || a.Length != b.Length) return -1;
            long ch = 0;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) ch++;
            return (double)ch / a.Length;
        }
        private static string Hash8(byte[] data)
        {
            using (var sha = SHA1.Create())
            {
                var h = sha.ComputeHash(data);
                var sb = new StringBuilder(8);
                for (int i = 0; i < 4; i++) sb.Append(h[i].ToString("x2"));
                return sb.ToString();
            }
        }
        private void SavePng(string name, byte[] bgra)
        {
            try
            {
                var bs = System.Windows.Media.Imaging.BitmapSource.Create(FrameW, FrameH, 96, 96,
                    System.Windows.Media.PixelFormats.Bgra32, null, bgra, FrameW * 4);
                var enc = new System.Windows.Media.Imaging.PngBitmapEncoder();
                enc.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bs));
                using (var fs = File.Create(Path.Combine(_ev, name))) enc.Save(fs);
            }
            catch (Exception ex) { _host.Log("PNG_FAIL " + name + " " + ex.Message); }
        }
        internal static double[] QToMatrix(double[] q)
        {
            double x = q[0], y = q[1], z = q[2], w = q[3];
            return new[]
            {
                1 - 2*(y*y + z*z), 2*(x*y - z*w),   2*(x*z + y*w),
                2*(x*y + z*w),     1 - 2*(x*x + z*z), 2*(y*z - x*w),
                2*(x*z - y*w),     2*(y*z + x*w),   1 - 2*(x*x + y*y)
            };
        }
        internal static double[] QAxis(double x, double y, double z, double deg)
        {
            double r = deg * Math.PI / 360.0; double n = Math.Sqrt(x*x + y*y + z*z);
            return new[] { x/n*Math.Sin(r), y/n*Math.Sin(r), z/n*Math.Sin(r), Math.Cos(r) };
        }
        // Boundary setter: R/p are CAMERA-TO-WORLD; ViewMatrix is WORLD-TO-CAMERA
        // (pixel-verified) → return the SDK inverse (see VCFrameProducer.cs header).
        // _entry is the RAW ViewMatrix (world-to-camera); restored un-inverted below.
        // _R_E/_P_E via VCFrameProducer.MatToRotPos are CAMERA-TO-WORLD.
        internal static Matrix RotPosToMat(double[] R, double[] p)
        {
            var m = new Matrix();
            m.Nx = R[0]; m.Ny = R[3]; m.Nz = R[6];
            m.Ox = R[1]; m.Oy = R[4]; m.Oz = R[7];
            m.Ax = R[2]; m.Ay = R[5]; m.Az = R[8];
            m.Px = p[0]; m.Py = p[1]; m.Pz = p[2];
            return m.Inverse();
        }
        internal static double[] Mul3(double[] a, double[] b)
        {
            var r = new double[9];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    r[i*3+j] = a[i*3]*b[j] + a[i*3+1]*b[3+j] + a[i*3+2]*b[6+j];
            return r;
        }
        internal static double[] Sub(double[] a, double[] b) { return new[] { a[0]-b[0], a[1]-b[1], a[2]-b[2] }; }
        private void Finish() { RestoreCamera(); try { _app?.SetRecordingMode(_win, false, 0, 0); } catch { } }

        /// <summary>Cancel from the UI thread (Exit / plugin Exit / failure teardown): stops the
        /// timer, restores entry camera (Gaze → raw ViewMatrix un-inverted → FOV) and recording.
        /// Cancellation never reports success/failure to the host — late callbacks are suppressed
        /// by the _canceled checks.</summary>
        public void CancelAndRestore()
        {
            _canceled = true;
            try { if (_timer != null) { _timer.Stop(); _timer = null; } } catch { }
            RestoreCamera();
            try { if (_app != null && _win != null) _app.SetRecordingMode(_win, false, 0, 0); } catch { }
            _host.Log("SWEEP_CANCELED_AND_RESTORED stage=" + _sweepStage);
        }

        private void RestoreCamera()
        {
            try
            {
                if (_cam == null) return;
                if (!double.IsNaN(_gazeEntry) && !double.IsInfinity(_gazeEntry))
                { try { _cam.GazeLength = _gazeEntry; } catch { } }
                _cam.ViewMatrix = _entry;   // RAW world-to-camera view — restored as-is, no inversion
                try { _cam.FieldOfViewY = _fovEntry; } catch { }
            }
            catch (Exception ex) { _host.Log("SWEEP_RESTORE_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }
    }
}
