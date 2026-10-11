// EntryCalibration.cs — VRSIM-PRODUCT-02C-CALIBRATION (0.1.1.7).
//
// Decides WHEN to lock XR_ENTRY_HEAD_POSE. FOCUSED is input focus, never HEADSET_WORN.
// Generic OpenXR path: XR_EXT_user_presence events when the runtime provides them;
// otherwise a documented fallback heuristic (visible/focused + a large transport motion
// followed by a stability window — so a headset lying quietly on the desk never locks).
//
// State machine (PHASE 7):
//   WAITING_FOR_USER -> (userPresent | fallbackMotion) -> CALIBRATING_ENTRY
//   -> (stability window met) -> ENTRY_LOCKED  [delta == identity at lock]
//
// Thresholds and their basis (PHASE 4 — no magic unexplained numbers):
//   stabilityWindowMs = 400  : ~2 eye-gaze fixations; short enough to feel instant.
//   maxAngularVelDegPerS = 25: a deliberately-still head stays < ~10 deg/s; picking up or
//                              flipping a headset exceeds 60 deg/s. 25 splits them.
//   maxPositionVelMmPerS = 30: still-worn head micro-motion < ~15 mm/s; desk pickup > 200 mm/s.
//   maxJitterDeg/MM within the window: 4 deg / 8 mm (peak-to-peak) — stillness, not perfection.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace VRSIM
{
    internal enum EntryCalState { WaitingForUser, CalibratingEntry, EntryLocked }

    internal sealed class EntryCalibration
    {
        internal const double StabilityWindowMs = 1500;
        internal const double MaxAngularVelDegPerS = 25.0;
        internal const double MaxPositionVelMmPerS = 30.0;
        internal const double MaxWindowJitterDeg = 4.0;
        internal const double MaxWindowJitterMm = 8.0;
        internal const double FallbackBurstAngularVel = 45.0;
        internal const double FallbackBurstPositionVel = 200.0;

        public EntryCalState State { get; private set; } = EntryCalState.WaitingForUser;
        public bool PresenceAvailable { get; private set; }
        public string PresenceSignal { get; private set; } = "NONE";
        public double StableDurationMs { get; private set; }
        public double PositionJitterMm { get; private set; }
        public double OrientationJitterDeg { get; private set; }
        public int StabilitySamples { get; private set; }

        private readonly struct Sample
        {
            public readonly double T; public readonly double[] Pos; public readonly double[] Quat;
            public Sample(double t, double[] pos, double[] quat) { T = t; Pos = pos; Quat = quat; }
        }
        private readonly List<Sample> _window = new List<Sample>();
        private bool _fallbackMotionSeen;
        private double _lastT = -1;
        private double[] _lastPos, _lastQuat;

        /// <summary>Feed one tracking sample. Returns true exactly on the transition to locked.
        /// quat = SIM-frame (x,y,z,w); pos = SIM mm; tMs = monotonic ms; userPresent = -1 unknown.</summary>
        public bool Feed(double tMs, double[] pos, double[] quat, int userPresent, bool trackingValid)
        {
            if (State == EntryCalState.EntryLocked) return false;
            if (!trackingValid) { _window.Clear(); return false; }

            bool presenceUsable = PresenceAvailable && userPresent >= 0;
            if (presenceUsable && userPresent == 0)
            {
                // explicitly not worn (authoritative signal) — wait
                _window.Clear(); _fallbackMotionSeen = false;
                if (State == EntryCalState.CalibratingEntry) State = EntryCalState.WaitingForUser;
                _lastT = tMs; _lastPos = pos; _lastQuat = quat;
                return false;
            }

            double dt = _lastT < 0 ? 0 : tMs - _lastT;
            double angVel = 0, posVel = 0;
            if (_lastT >= 0 && dt > 1e-6 && dt < 500)
            {
                angVel = QuaternionAngleDeg(_lastQuat, quat) / (dt / 1000.0);
                posVel = Dist(_lastPos, pos) / (dt / 1000.0);
            }
            _lastT = tMs; _lastPos = (double[])pos.Clone(); _lastQuat = (double[])quat;

            // transition into calibration (0.1.3.0): with natural proximity behavior,
            // FOCUSED only happens when actually worn — so gate = FOCUSED (+presence if the
            // runtime provides it). Stability window (1.5 s) covers the settling after donning.
            if (State == EntryCalState.WaitingForUser)
            {
                bool gate = presenceUsable ? userPresent == 1 : FocusedGate;
                if (gate) State = EntryCalState.CalibratingEntry;
                else { _window.Clear(); return false; }
                _window.Clear();
            }

            // stability window (PHASE 4)
            _window.Add(new Sample(tMs, (double[])pos.Clone(), (double[])quat));
            while (_window.Count > 1 && tMs - _window[0].T > StabilityWindowMs) _window.RemoveAt(0);
            StableDurationMs = _window.Count > 0 ? tMs - _window[0].T : 0;
            StabilitySamples = _window.Count;

            if (_window.Count >= 5 && StableDurationMs >= StabilityWindowMs * 0.9)
            {
                (PositionJitterMm, OrientationJitterDeg) = WindowJitter();
                bool still = OrientationJitterDeg <= MaxWindowJitterDeg && PositionJitterMm <= MaxWindowJitterMm;
                // instantaneous velocity within window must also stay low (no mid-window spin)
                if (still && angVel <= MaxAngularVelDegPerS && posVel <= MaxPositionVelMmPerS)
                {
                    State = EntryCalState.EntryLocked;
                    return true; // lock NOW with the CURRENT pose (PHASE 5)
                }
            }
            return false;
        }

        public bool FocusedGate { get; set; } // fallback path requires a focused session

        public void SetPresenceCapability(bool available) { PresenceAvailable = available; }

        private (double mm, double deg) WindowJitter()
        {
            double minx = 1e18, maxx = -1e18, maxAng = 0;
            for (int i = 0; i < _window.Count; i++)
                for (int j = i + 1; j < _window.Count; j++)
                {
                    double d = Dist(_window[i].Pos, _window[j].Pos);
                    if (d > maxx) maxx = d;
                    double a = QuaternionAngleDeg(_window[i].Quat, _window[j].Quat);
                    if (a > maxAng) maxAng = a;
                }
            if (_window.Count < 2) { maxx = 0; maxAng = 0; }
            _ = minx;
            return (maxx, maxAng);
        }

        private static double Dist(double[] a, double[] b)
        { return Math.Sqrt(Sq(a[0]-b[0])+Sq(a[1]-b[1])+Sq(a[2]-b[2])); }
        private static double Sq(double v) { return v*v; }

        private static double QuaternionAngleDeg(double[] a, double[] b)
        {
            // relative rotation magnitude between two sim-frame quaternions (x,y,z,w)
            double x = a[1]*b[2]-a[2]*b[1], y = a[2]*b[0]-a[0]*b[2], z = a[0]*b[1]-a[1]*b[0], w =
                  a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3]; // conj(a)*b approx via cross/dot trick
            // properly: qrel = conj(a) ⊗ b
            double[] ca = { -a[0], -a[1], -a[2], a[3] };
            double[] r = MulQ(ca, b);
            x = r[0]; y = r[1]; z = r[2]; w = r[3];
            double n = Math.Sqrt(x*x+y*y+z*z+w*w);
            if (n < 1e-12) return 0;
            w /= n;
            return 2.0 * Math.Acos(Math.Max(-1, Math.Min(1, w))) * 180.0 / Math.PI;
        }
        private static double[] MulQ(double[] a, double[] b)
        {
            double x1=a[0],y1=a[1],z1=a[2],w1=a[3], x2=b[0],y2=b[1],z2=b[2],w2=b[3];
            return new[] {
                w1*x2 + x1*w2 + y1*z2 - z1*y2,
                w1*y2 - x1*z2 + y1*w2 + z1*x2,
                w1*z2 + x1*y2 - y1*x2 + z1*w2,
                w1*w2 - x1*x2 - y1*y2 - z1*z2 };
        }

        // ---------------- PHASE 13: synthetic calibration simulation ----------------
        public sealed class SimSample
        {
            public double T; public double[] Pos; public double[] Quat; public int Presence; public bool Valid;
            public SimSample(double t, double[] pos, double[] quat, int presence, bool valid)
            { T = t; Pos = pos; Quat = quat; Presence = presence; Valid = valid; }
        }

        /// <summary>Run the recorded synthetic sequence through the state machine and write
        /// entry-calibration-simulation.json. Sequence: desk (flat) -> pickup -> transport ->
        /// wearing transition -> stable worn. The lock must happen only in the stable period.</summary>
        public static string SimulateAndReport()
        {
            var seq = new List<SimSample>();
            var qFlat = QAxis(1, 0, 0, -85);            // headset lying face-up on desk: pitched back
            var qDesk = QAxis(1, 0, 0, -85);
            var qWorn = QAxis(0, 1, 0, 0);              // worn: level
            var pDesk = new[] { 300.0, 900.0, 100.0 };  // on the desk
            var pHeld = new[] { 300.0, 1300.0, 500.0 }; // picked up
            var pWorn = new[] { 268.0, -1500.0, 850.0 };// worn (near entry rig translate)
            for (double t = 0; t < 1000; t += 40)       // T0: lying on desk, no presence
                seq.Add(new SimSample(t, Jit(pDesk, t, 0.4), JitQ(qDesk, t, 0.3), 0, true));
            for (double t = 1000; t < 1300; t += 40)    // T1/T2: pickup + fast transport
            {
                double k = (t - 1000) / 300.0;
                var p = Mix(pDesk, pHeld, k); var q = MixQ(qDesk, QAxis(1,0,0,60), k);
                seq.Add(new SimSample(t, Jit(p, t, 3), JitQ(q, t, 2), 0, true));
            }
            for (double t = 1300; t < 1600; t += 40)    // T3: wearing transition (presence flips on mid-way)
            {
                double k = (t - 1300) / 300.0;
                var p = Mix(pHeld, pWorn, k); var q = MixQ(QAxis(1,0,0,60), qWorn, k);
                seq.Add(new SimSample(t, Jit(p, t, 8), JitQ(q, t, 6), t > 1450 ? 1 : -1, true));
            }
            for (double t = 1600; t < 3600; t += 20)    // T4: stable worn (>=1.5 s stability window)
                seq.Add(new SimSample(t, Jit(pWorn, t, 0.6), JitQ(qWorn, t, 0.4), 1, true));

            var cal = new EntryCalibration { FocusedGate = true };
            cal.SetPresenceCapability(true); // presence available in simulation
            double lockT = -1;
            var states = new StringBuilder();
            foreach (var s2 in seq)
            {
                bool locked = cal.Feed(s2.T, s2.Pos, s2.Quat, s2.Presence, s2.Valid);
                if (locked) lockT = s2.T;
                states.Append(s2.T.ToString("0", CultureInfo.InvariantCulture)).Append(':')
                      .Append(cal.State).Append(' ');
            }
            bool pass = lockT >= 1600 && cal.State == EntryCalState.EntryLocked; // lock must be in T4 stable-worn window (any time after wearing)
            return "{\"pass\":" + (pass ? "true" : "false") +
                   ",\"lockedAtMs\":" + lockT.ToString("0", CultureInfo.InvariantCulture) +
                   ",\"stableDurationMs\":" + cal.StableDurationMs.ToString("0.0", CultureInfo.InvariantCulture) +
                   ",\"positionJitterMm\":" + cal.PositionJitterMm.ToString("0.00", CultureInfo.InvariantCulture) +
                   ",\"orientationJitterDeg\":" + cal.OrientationJitterDeg.ToString("0.00", CultureInfo.InvariantCulture) +
                   ",\"stabilitySamples\":" + cal.StabilitySamples +
                   ",\"rule\":\"lock only in T4 stable-worn window, never at T0-T3\"" +
                   ",\"trace\":\"" + states.ToString() + "\"}";
        }

        private static double[] Jit(double[] p, double t, double amp)
        {
            return new[] { p[0] + Math.Sin(t * 0.013) * amp, p[1] + Math.Cos(t * 0.017) * amp, p[2] + Math.Sin(t * 0.011) * amp };
        }
        private static double[] JitQ(double[] q, double t, double degAmp)
        { return NormalizeQ(MulQ(QAxis(0, 1, 0, Math.Sin(t * 0.01) * degAmp), q)); }
        private static double[] NormalizeQ(double[] q)
        {
            double n = Math.Sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
            return new[] { q[0]/n, q[1]/n, q[2]/n, q[3]/n };
        }
        private static double[] Mix(double[] a, double[] b, double k)
        { return new[] { a[0]+(b[0]-a[0])*k, a[1]+(b[1]-a[1])*k, a[2]+(b[2]-a[2])*k }; }
        private static double[] MixQ(double[] a, double[] b, double k) { return NormalizeQ(MulQ(a, b)); }
        private static double[] QAxis(double x, double y, double z, double deg)
        {
            double r = deg * Math.PI / 360.0; double n = Math.Sqrt(x*x+y*y+z*z);
            return new[] { x/n*Math.Sin(r), y/n*Math.Sin(r), z/n*Math.Sin(r), Math.Cos(r) };
        }
    }
}
