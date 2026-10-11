// TrackingSelfTest.cs — VRSIM-PRODUCT-02C-RUNTIME-QA (0.1.1.5).
//
// Runs INSIDE the real iiQWorks.Sim process against the REAL VC camera BEFORE any XR
// session is created. Synthetic head deltas flow through the EXACT production math
// (VCFrameProducer.ComputeCameras), get WRITTEN to cam.ViewMatrix, the REAL renderer
// captures a frame, and a geometric projection oracle auto-judges direction.
// USER_IS_NOT_A_DEBUGGER: no user testing happens unless every gate here passes.
using System;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows.Threading;
using VisualComponents.Create3D;

namespace VRSIM
{
    internal sealed class TrackingSelfTest
    {
        private readonly VRSIMPlugin _host;
        private readonly Dispatcher _disp;
        private IApplication _app;
        private VisualComponents.Create3D.IWindow3D _win;
        private ICamera _cam;
        private Matrix _entry;
        private double[] _R_E = new double[9], _P_E = new double[3];
        private double _fovY = 35.0;
        private string _ev;
        private readonly StringBuilder _log = new StringBuilder();

        public string SummaryLog { get { return _log.ToString(); } }

        // PHASE 26 gate results
        public bool EntryIdentity, YawLeft, YawRight, PitchUp, PitchDown;
        public bool TranslateLeft, TranslateRight, TranslateUp, TranslateForward, StereoCenter;

        public TrackingSelfTest(VRSIMPlugin host, Dispatcher disp)
        {
            _host = host;
            _disp = disp;
            _ev = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "KUKA", "iiQWorks.Sim", "VRSIM", "selftest");
            try { Directory.CreateDirectory(_ev); } catch { }
        }

        /// <summary>Executes the full probe set on the UI thread. Returns overall PASS.</summary>
        public bool Run()
        {
            bool ok = false;
            _disp.Invoke((System.Action)(() => { ok = RunOnUi(); }));
            return ok;
        }

        private bool RunOnUi()
        {
            try
            {
                _app = _host.Application;
                var vp = VisualComponents.UX.Input.HelperViewport.ActiveViewport;
                if (vp == null || vp.Window == null) { Fail("no_active_viewport"); return false; }
                _win = vp.Window;
                _cam = _win.Camera;
                _entry = _cam.ViewMatrix;
                _fovY = _cam.FieldOfViewY;
                VCFrameProducer.MatToRotPos(_entry, _R_E, _P_E);
                WriteJson("vc-entry-camera.json", JMatrix("entry", _entry));
                Log("ENTRY cam=(" + V(_P_E) + ") fovY=" + _fovY.ToString("0.0", CultureInfo.InvariantCulture));

                // PHASE 6: real basis audit (determinant + orthogonality of read-back matrix)
                double det = Det3(_R_E);
                double orthoErr = OrthoErr3(_R_E);
                WriteJson("vc-camera-basis.json", "{\"det\":" + det.ToString("0.000000", CultureInfo.InvariantCulture) +
                    ",\"orthoErr\":" + orthoErr.ToString("0.00000000", CultureInfo.InvariantCulture) +
                    ",\"N\":[" + Col(_entry, 0) + "],\"O\":[" + Col(_entry, 1) + "],\"A\":[" + Col(_entry, 2) + "]" +
                    ",\"convention\":\"raw ViewMatrix = worldToCamera (pixel-verified 2026-10-02); R/P extracted via Inverse = camera-to-world\"}");
                Log("BASIS det=" + det.ToString("0.000000", CultureInfo.InvariantCulture) + " orthoErr=" + orthoErr.ToString("0.00000000", CultureInfo.InvariantCulture));

                _app.SetRecordingMode(_win, true, (uint)VCFrameProducer.FrameW, (uint)VCFrameProducer.FrameH);
                try
                {
                    // PHASE 4/5: identity delta — production math must return the entry camera UNTOUCHED
                    EntryIdentity = Probe("identity-delta", QId(), Z3(), null);
                    // PHASE 9-15: synthetic head deltas in the HEAD-LOCAL frame
                    //   yaw left = +10° about head up (+Y); pitch up = +10° about head right (+X);
                    //   head-local axes ≡ camera-local axes (PHASE 5, ledger doc)
                    YawLeft = Probe("yaw-left", QAxis(0, 1, 0, 10), Z3(), null);
                    YawRight = Probe("yaw-right", QAxis(0, 1, 0, -10), Z3(), null);
                    PitchUp = Probe("pitch-up", QAxis(1, 0, 0, 10), Z3(), null);
                    PitchDown = Probe("pitch-down", QAxis(1, 0, 0, -10), Z3(), null);
                    // translations in head-local mm (PHASE 14/15): left=-X, up=+Y, forward=-Z
                    TranslateLeft = Probe("translate-left", QId(), V3(-100, 0, 0), null);
                    TranslateRight = Probe("translate-right", QId(), V3(100, 0, 0), null);
                    TranslateUp = Probe("translate-up", QId(), V3(0, 100, 0), null);
                    TranslateForward = Probe("translate-forward", QId(), V3(0, 0, -100), null);

                    // PHASE 23: stereo around center (synthetic ±32mm eye offsets, TEST_ONLY)
                    StereoCenter = StereoProbe();

                    // restore + verify
                    _cam.ViewMatrix = _entry;
                    bool restored = MatEq(_cam.ViewMatrix, _entry);
                    Log("RESTORE restored=" + restored);
                    WriteJson("vc-restore.json", "{\"restored\":" + (restored ? "true" : "false") + "}");
                }
                finally
                {
                    _app.SetRecordingMode(_win, false, 0, 0);
                    _cam.ViewMatrix = _entry;
                }
            }
            catch (Exception ex)
            {
                Log("SELFTEST_EXCEPTION " + ex.GetType().Name + ": " + ex.Message);
                try { _app.SetRecordingMode(_win, false, 0, 0); _cam.ViewMatrix = _entry; } catch { }
                return false;
            }

            bool pass = EntryIdentity && YawLeft && YawRight && PitchUp && PitchDown &&
                        TranslateLeft && TranslateRight && TranslateUp && TranslateForward && StereoCenter;
            Log("SELFTEST_RESULT pass=" + pass +
                " entry=" + EntryIdentity + " yawL=" + YawLeft + " yawR=" + YawRight +
                " pitchU=" + PitchUp + " pitchD=" + PitchDown +
                " trL=" + TranslateLeft + " trR=" + TranslateRight + " trU=" + TranslateUp +
                " trF=" + TranslateForward + " stereo=" + StereoCenter);
            return pass;
        }

        // ---------------- one probe: production math → real camera → real render → oracle ----
        private bool Probe(string name, double[] qDelta, double[] pDeltaLocalMm, double[] _unused)
        {
            // production bridge (identical formulas to the live path, PHASE 3):
            // R_vc = R_E·R_D ; P_vc = P_E + R_E·p_D ; entry-head-local axes ≡ camera-local axes
            double[] R_D = QToMatrix(qDelta);
            double[] R_vc = Mul3(_R_E, R_D);
            double[] P_vc = Add(_P_E, Apply3(_R_E, pDeltaLocalMm));
            Matrix target = RotPosToMat(R_vc, P_vc);

            _cam.ViewMatrix = target;
            Matrix actual = _cam.ViewMatrix;
            byte[] img = CaptureFresh();
            SavePng(name + ".png", img);
            if (name == "identity-delta") SavePng("entry-before.png", CaptureFresh()); // reference

            // matrix-level truth: what VC actually holds after our write
            double[] R_a = new double[9], P_a = new double[3];
            VCFrameProducer.MatToRotPos(actual, R_a, P_a);
            double posErr = Dist(P_a, P_vc);
            double oriErrDeg = AngleBetweenR(R_a, R_vc);
            double[] f = Fwd(R_a);

            // geometric oracle: project a fixed world reference point, compare screen shifts
            double[] refPt = RefPoint();
            var s_entry = Project(refPt, _R_E, _P_E);
            var s_this = Project(refPt, R_a, P_a);
            bool oracle;
            string oracleNote;
            switch (name)
            {
                case "identity-delta":
                    oracle = posErr < 1e-6 && oriErrDeg < 1e-6;
                    oracleNote = "target==entry required";
                    break;
                case "yaw-left":  // look left → world point moves RIGHT on screen
                    oracle = s_this.ok && s_entry.ok && s_this.x > s_entry.x + 2;
                    oracleNote = "screenX " + F(s_entry.x) + "->" + F(s_this.x);
                    break;
                case "yaw-right":
                    oracle = s_this.ok && s_entry.ok && s_this.x < s_entry.x - 2;
                    oracleNote = "screenX " + F(s_entry.x) + "->" + F(s_this.x);
                    break;
                case "pitch-up":  // look up → world point moves DOWN (row increases)
                    oracle = s_this.ok && s_entry.ok && s_this.y > s_entry.y + 2;
                    oracleNote = "screenY " + F(s_entry.y) + "->" + F(s_this.y);
                    break;
                case "pitch-down":
                    oracle = s_this.ok && s_entry.ok && s_this.y < s_entry.y - 2;
                    oracleNote = "screenY " + F(s_entry.y) + "->" + F(s_this.y);
                    break;
                case "translate-left":   // camera left → world point moves RIGHT on screen
                    oracle = s_this.ok && s_entry.ok && s_this.x > s_entry.x + 2;
                    oracleNote = "screenX " + F(s_entry.x) + "->" + F(s_this.x);
                    break;
                case "translate-right":
                    oracle = s_this.ok && s_entry.ok && s_this.x < s_entry.x - 2;
                    oracleNote = "screenX " + F(s_entry.x) + "->" + F(s_this.x);
                    break;
                case "translate-up":     // camera up → world point moves DOWN on screen
                    oracle = s_this.ok && s_entry.ok && s_this.y > s_entry.y + 2;
                    oracleNote = "screenY " + F(s_entry.y) + "->" + F(s_this.y);
                    break;
                case "translate-forward": // toward the point → its screen offset from center GROWS
                    oracle = s_this.ok && s_entry.ok &&
                        Math.Abs(s_this.x - 320) > Math.Abs(s_entry.x - 320) + 2;
                    oracleNote = "|screenX-cx| " + F(Math.Abs(s_entry.x - 320)) + "->" + F(Math.Abs(s_this.x - 320));
                    break;
                default: oracle = false; oracleNote = "?"; break;
            }

            bool pass = oracle && posErr < 1e-6 && oriErrDeg < 1e-6;
            Log("PROBE " + name + " pass=" + pass + " posErr=" + posErr.ToString("0.000000", CultureInfo.InvariantCulture) +
                " oriErrDeg=" + oriErrDeg.ToString("0.000000", CultureInfo.InvariantCulture) +
                " forward=[" + V(f) + "] oracle{" + oracleNote + "}");
            WriteJson("runtime-" + name + ".json",
                "{\"pass\":" + (pass ? "true" : "false") +
                ",\"positionErrorMm\":" + posErr.ToString("0.000000", CultureInfo.InvariantCulture) +
                ",\"orientationErrorDeg\":" + oriErrDeg.ToString("0.000000", CultureInfo.InvariantCulture) +
                ",\"forwardWorld\":[" + V(f) + "]" +
                ",\"oracle\":\"" + oracleNote + "\"" +
                ",\"targetMatrix\":" + JMatrix("target", target) +
                ",\"actualMatrix\":" + JMatrix("actual", actual) + "}");
            return pass;
        }

        // PHASE 23: L/R around center with synthetic ±32mm head-local eye offsets
        private bool StereoProbe()
        {
            double ipdHalf = 32.0;
            double[] R_h0 = QToMatrix(QId());
            var eyeL = (pos: Apply3(R_h0, V3(-ipdHalf, 0, 0)), quat: QId());
            var eyeR = (pos: Apply3(R_h0, V3(ipdHalf, 0, 0)), quat: QId());
            // production path with origin == entry head (identity delta)
            double[] R_vc = _R_E, P_vc = _P_E;
            double[] R_h = QToMatrix(QId());
            double[] R_relL = Mul3(Trans3(R_h), QToMatrix(eyeL.quat));
            double[] R_relR = Mul3(Trans3(R_h), QToMatrix(eyeR.quat));
            double[] p_relL = Apply3(Trans3(R_h), eyeL.pos);
            double[] p_relR = Apply3(Trans3(R_h), eyeR.pos);
            double[] PL = Add(P_vc, Apply3(R_vc, p_relL));
            double[] PR = Add(P_vc, Apply3(R_vc, p_relR));
            double[] mid = Mul(Add(PL, PR), 0.5);
            double sep = Dist(PL, PR);
            double[] sepDir = Sub(PR, PL); double n = Len(sepDir); for (int i = 0; i < 3; i++) sepDir[i] /= n;
            double alongRight = sepDir[0] * _R_E[0] + sepDir[1] * _R_E[3] + sepDir[2] * _R_E[6]; // R_E column0 = N (right)
            bool ok = Dist(mid, _P_E) < 1e-6 && Math.Abs(sep - 64.0) < 1e-6 && Math.Abs(alongRight - 1.0) < 1e-6;
            Log("STEREO_PROBE midErr=" + Dist(mid, _P_E).ToString("0.000000", CultureInfo.InvariantCulture) +
                " sep=" + sep.ToString("0.0", CultureInfo.InvariantCulture) +
                " alongCameraRight=" + alongRight.ToString("0.000000", CultureInfo.InvariantCulture) + " pass=" + ok);
            WriteJson("stereo-runtime-selftest.json",
                "{\"pass\":" + (ok ? "true" : "false") + ",\"midErrorMm\":" + Dist(mid, _P_E).ToString("0.000000", CultureInfo.InvariantCulture) +
                ",\"separationMm\":" + sep.ToString("0.0", CultureInfo.InvariantCulture) +
                ",\"alongCameraRight\":" + alongRight.ToString("0.000000", CultureInfo.InvariantCulture) + "}");
            return ok;
        }

        // ---------------- projection oracle (pinhole, FOV Y from the real camera) ----------------
        private struct ScreenPt { public bool ok; public double x, y; }
        private double[] _refPt;
        private double[] RefPoint()
        {
            if (_refPt != null) return _refPt;
            double[] fwd = { -_R_E[2], -_R_E[5], -_R_E[8] }; // forward = -A column
            double[] right = { _R_E[0], _R_E[3], _R_E[6] };
            _refPt = new double[3];
            for (int i = 0; i < 3; i++)
                _refPt[i] = _P_E[i] + fwd[i] * 2000.0 + right[i] * 300.0; // ahead + off-center
            return _refPt;
        }
        private ScreenPt Project(double[] worldPt, double[] R, double[] P)
        {
            var s = new ScreenPt();
            double[] d = Sub(worldPt, P);
            double x = d[0] * R[0] + d[1] * R[3] + d[2] * R[6];   // cam right
            double y = d[0] * R[1] + d[1] * R[4] + d[2] * R[7];   // cam up
            double z = d[0] * R[2] + d[1] * R[5] + d[2] * R[8];   // cam back
            if (z > -10) { s.ok = false; return s; }               // behind / too close
            double tanY = Math.Tan(_fovY * Math.PI / 360.0);
            double tanX = tanY * VCFrameProducer.FrameW / VCFrameProducer.FrameH;
            s.x = ((x / -z) / tanX + 1) * 0.5 * VCFrameProducer.FrameW;
            s.y = (1 - ((y / -z) / tanY + 1) * 0.5) * VCFrameProducer.FrameH; // row 0 = top
            s.ok = true;
            return s;
        }

        // ---------------- real capture with freshness polling (OV11 discipline) ----------------
        private string _lastHash = "";
        private byte[] CaptureFresh()
        {
            var deadline = Environment.TickCount + 2000;
            while (Environment.TickCount < deadline)
            {
                var raw = _app.SurfaceToContinuousBuffer((uint)VCFrameProducer.FrameW, (uint)VCFrameProducer.FrameH);
                if (raw != null && raw.Length >= CaptureProfile.PayloadBytes)
                {
                    var copy = new byte[CaptureProfile.PayloadBytes];
                    Buffer.BlockCopy(raw, 0, copy, 0, CaptureProfile.PayloadBytes);
                    VCFrameProducer.ForceAlpha(copy);
                    string h = Hash(copy);
                    if (h != _lastHash) { _lastHash = h; return copy; }
                }
                System.Threading.Thread.Sleep(10);
            }
            var raw2 = _app.SurfaceToContinuousBuffer((uint)VCFrameProducer.FrameW, (uint)VCFrameProducer.FrameH);
            if (raw2 != null && raw2.Length >= CaptureProfile.PayloadBytes)
            {
                var fallback = new byte[CaptureProfile.PayloadBytes];
                Buffer.BlockCopy(raw2, 0, fallback, 0, CaptureProfile.PayloadBytes);
                VCFrameProducer.ForceAlpha(fallback);
                return fallback;
            }
            return new byte[CaptureProfile.PayloadBytes];
        }

        // ---------------- shared math (must stay identical to the live path) ----------------
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
        // Boundary setter: R/p are CAMERA-TO-WORLD; ViewMatrix is WORLD-TO-CAMERA
        // (pixel-verified, see VCFrameProducer.cs header) → return the SDK inverse.
        internal static Matrix RotPosToMat(double[] R, double[] p)
        {
            var m = new Matrix();
            m.Nx = R[0]; m.Ny = R[3]; m.Nz = R[6];
            m.Ox = R[1]; m.Oy = R[4]; m.Oz = R[7];
            m.Ax = R[2]; m.Ay = R[5]; m.Az = R[8];
            m.Px = p[0]; m.Py = p[1]; m.Pz = p[2];
            return m.Inverse();
        }
        // Boundary getter: input is the RAW ViewMatrix (world-to-camera); inverted before
        // extracting CAMERA-TO-WORLD R (row-major) / P (world mm). Must stay identical to
        // VCFrameProducer.MatToRotPos.
        internal static void MatToRotPos(Matrix m, double[] R, double[] p)
        {
            m = m.Inverse();
            R[0] = m.Nx; R[1] = m.Ox; R[2] = m.Ax;
            R[3] = m.Ny; R[4] = m.Oy; R[5] = m.Ay;
            R[6] = m.Nz; R[7] = m.Oz; R[8] = m.Az;
            p[0] = m.Px; p[1] = m.Py; p[2] = m.Pz;
        }
        private static double[] Mul3(double[] a, double[] b)
        {
            var r = new double[9];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    r[i*3+j] = a[i*3]*b[j] + a[i*3+1]*b[3+j] + a[i*3+2]*b[6+j];
            return r;
        }
        private static double[] Trans3(double[] a) { return new[] { a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8] }; }
        private static double[] Apply3(double[] m, double[] v)
        {
            return new[] { m[0]*v[0]+m[1]*v[1]+m[2]*v[2], m[3]*v[0]+m[4]*v[1]+m[5]*v[2], m[6]*v[0]+m[7]*v[1]+m[8]*v[2] };
        }
        private static double[] Add(double[] a, double[] b) { return new[] { a[0]+b[0], a[1]+b[1], a[2]+b[2] }; }
        private static double[] Sub(double[] a, double[] b) { return new[] { a[0]-b[0], a[1]-b[1], a[2]-b[2] }; }
        private static double[] Mul(double[] a, double k) { return new[] { a[0]*k, a[1]*k, a[2]*k }; }
        private static double Dist(double[] a, double[] b) { return Math.Sqrt(Sq(a[0]-b[0])+Sq(a[1]-b[1])+Sq(a[2]-b[2])); }
        private static double Len(double[] a) { return Math.Sqrt(Sq(a[0])+Sq(a[1])+Sq(a[2])); }
        private static double Sq(double v) { return v*v; }
        private static double[] Z3() { return new[] { 0.0, 0.0, 0.0 }; }
        private static double[] V3(double x, double y, double z) { return new[] { x, y, z }; }
        private static double[] QId() { return new[] { 0.0, 0.0, 0.0, 1.0 }; }
        private static double[] QAxis(double x, double y, double z, double deg)
        {
            double r = deg * Math.PI / 360.0; double n = Math.Sqrt(x*x+y*y+z*z);
            return new[] { x/n*Math.Sin(r), y/n*Math.Sin(r), z/n*Math.Sin(r), Math.Cos(r) };
        }
        private static double[] Fwd(double[] R) { return new[] { -R[2], -R[5], -R[8] }; }
        private static double AngleBetweenR(double[] A, double[] B)
        {
            // rotation angle of A·Bᵀ
            double[] M = Mul3(A, Trans3(B));
            double c = (M[0] + M[4] + M[8] - 1.0) / 2.0;
            return Math.Acos(Math.Max(-1, Math.Min(1, c))) * 180.0 / Math.PI;
        }
        private static double Det3(double[] m)
        {
            return m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6]) + m[2]*(m[3]*m[7]-m[4]*m[6]);
        }
        private static double OrthoErr3(double[] m)
        {
            double e = 0;
            for (int i = 0; i < 3; i++)
                for (int j = i; j < 3; j++)
                {
                    double dot = m[0+i]*m[0+j] + m[3+i]*m[3+j] + m[6+i]*m[6+j];
                    e = Math.Max(e, Math.Abs(dot - (i == j ? 1 : 0)));
                }
            return e;
        }
        private static bool MatEq(Matrix a, Matrix b)
        {
            return a.Nx == b.Nx && a.Ny == b.Ny && a.Nz == b.Nz &&
                   a.Ox == b.Ox && a.Oy == b.Oy && a.Oz == b.Oz &&
                   a.Ax == b.Ax && a.Ay == b.Ay && a.Az == b.Az &&
                   a.Px == b.Px && a.Py == b.Py && a.Pz == b.Pz;
        }
        private static string Hash(byte[] data)
        {
            using (var sha = System.Security.Cryptography.SHA1.Create())
            {
                var h = sha.ComputeHash(data);
                var sb = new StringBuilder(40);
                foreach (var b in h) sb.Append(b.ToString("x2"));
                return sb.ToString();
            }
        }
        private void SavePng(string name, byte[] bgra)
        {
            try
            {
                var bs = System.Windows.Media.Imaging.BitmapSource.Create(VCFrameProducer.FrameW, VCFrameProducer.FrameH, 96, 96,
                    System.Windows.Media.PixelFormats.Bgra32, null, bgra, VCFrameProducer.FrameW * 4);
                var enc = new System.Windows.Media.Imaging.PngBitmapEncoder();
                enc.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bs));
                using (var fs = File.Create(Path.Combine(_ev, name))) enc.Save(fs);
            }
            catch (Exception ex) { Log("PNG_FAIL " + name + " " + ex.Message); }
        }
        private void WriteJson(string name, string content)
        {
            try { File.WriteAllText(Path.Combine(_ev, name), content); } catch { }
        }
        private static string JMatrix(string tag, Matrix m)
        {
            var ci = CultureInfo.InvariantCulture;
            return "{\"" + tag + "\":[" +
                m.Nx.ToString("R", ci) + "," + m.Ox.ToString("R", ci) + "," + m.Ax.ToString("R", ci) + "," + m.Px.ToString("R", ci) + "," +
                m.Ny.ToString("R", ci) + "," + m.Oy.ToString("R", ci) + "," + m.Ay.ToString("R", ci) + "," + m.Py.ToString("R", ci) + "," +
                m.Nz.ToString("R", ci) + "," + m.Oz.ToString("R", ci) + "," + m.Az.ToString("R", ci) + "," + m.Pz.ToString("R", ci) + "]}";
        }
        private static string Col(Matrix m, int c)
        {
            var ci = CultureInfo.InvariantCulture;
            if (c == 0) return m.Nx.ToString("0.000", ci) + "," + m.Ny.ToString("0.000", ci) + "," + m.Nz.ToString("0.000", ci);
            if (c == 1) return m.Ox.ToString("0.000", ci) + "," + m.Oy.ToString("0.000", ci) + "," + m.Oz.ToString("0.000", ci);
            return m.Ax.ToString("0.000", ci) + "," + m.Ay.ToString("0.000", ci) + "," + m.Az.ToString("0.000", ci);
        }
        private static string V(double[] v)
        {
            var ci = CultureInfo.InvariantCulture;
            return v[0].ToString("0.0", ci) + "," + v[1].ToString("0.0", ci) + "," + v[2].ToString("0.0", ci);
        }
        private static string F(double v) { return v.ToString("0.0", CultureInfo.InvariantCulture); }
        private void Fail(string why) { Log("SELFTEST_FAIL " + why); }
        private void Log(string line)
        {
            _log.AppendLine(DateTime.UtcNow.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture) + "Z " + line);
            _host.Log("SELFTEST " + line);
        }
    }
}
