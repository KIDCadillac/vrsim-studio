// VRSIMPlugin.cs — VRSIM product plugin for iiQWorks.Sim 1.3 (VRSIM-PRODUCT-01).
//
// Product flow: user opens iiQWorks.Sim, clicks Enter VR on the VRSIM ribbon tab,
// puts on the HMD, and sees the VRSIM VR MODE. No external EXE is launched —
// the native backend (VRSIM.Native.dll) runs OpenXR INSIDE this process so the
// session belongs to the SIM product lifecycle.
//
// This round (PRODUCT-01) renders a synthetic quad (magenta/cyan checkerboards +
// white cross) — visual hold only, no SIM scene content yet (PRODUCT-02).
//
// Evidence channel: %LOCALAPPDATA%\KUKA\iiQWorks.Sim\VRSIM\vr-plugin.log plus
// diagnostics.json holding inProcessSessionReachedVisible (the key PRODUCT-01 gate).
using System;
using System.ComponentModel.Composition;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using VisualComponents.Create3D;
using VisualComponents.UX.Shared;

[assembly: System.Reflection.AssemblyVersion("0.1.4.0")]
[assembly: System.Reflection.AssemblyFileVersion("0.1.4.0")]
[assembly: System.Reflection.AssemblyTitle("VRSIM")]

namespace VRSIM
{
    // RegisterSite(Tab) only creates a *site*; RibbonViewModel matches each tab site against
    // composed IRibbonTab parts by Id and silently drops sites with no matching part
    // (proven recipe: interactive-physics-grab RibbonTab.cs).
    [Export(typeof(IRibbonTab))]
    public sealed class VRSIMRibbonTab : IRibbonTab
    {
        public const string TabId = "VRSIMTab";
        public string Name { get { return TabId; } }
        public string Header { get { return "VRSIM"; } }
        public string Id { get { return TabId; } }
        public string ContextualTabGroupKey { get { return ""; } }
    }

    [Export(typeof(IPlugin))]
    [PartCreationPolicy(CreationPolicy.Shared)]
    public sealed partial class VRSIMPlugin : IPlugin
    {
        [Import] public IApplication Application { get; set; }
        [Import] public IUXConfiguration Ux { get; set; }
        [Import] public ICommandRegistry Commands { get; set; }

        private const string PluginVersion = "0.1.4.0";
#if VRSIM_NATIVE_QA
        private const string NativeDllName = "VRSIM.Native.QA.dll";
#else
        private const string NativeDllName = "VRSIM.Native.dll";
#endif

        private IActionItem _enter, _exit;
        private IActionItem _runSweep, _monitorBtn;   // 0.1.3.5: diagnostic sweep + live monitor
        private VRMonitorWindow _monitor;             // UI thread, single instance
        private bool _diagnosticFlow;                 // current session entered via 运行技术扫摆
        private bool _initialized, _busy, _vrActive;
        private string _logDir, _logPath, _diagPath;
        private string _nativeDir;
        private System.Windows.Threading.Dispatcher _disp;
        private System.Windows.Threading.DispatcherTimer _poll;
        private IMessageService _messages;
        private int _lastState = -1;
        private VCFrameProducer _producer;
        private VRSIM.XR.XRDeviceManager _deviceManager;
        private VRSIM.XR.KnownDeviceRegistry _knownRegistry;
        private bool _visibleAnnounced;
        private bool _quadAnnounced;
        private DateTime _enterAt;

        // READYNESS FIX (2026-10-02, PM-authorized): the Enter flow never sleeps the UI
        // dispatcher any more; readiness is awaited on a DispatcherTimer and only
        // VISIBLE(5)/FOCUSED(6) may start the sweep. _generation invalidates late
        // Task/timer/controller callbacks after Exit / plugin Exit / re-Enter.
        private int _generation;
        private volatile bool _creating;        // native Create/Start running in background
        private volatile bool _exitRequested;   // Exit queued while _creating
        private volatile bool _readyWaiting;    // DispatcherTimer readiness wait active
        private System.Windows.Threading.DispatcherTimer _readyTimer;
        private DateTime _readyStartedUtc;
        private int _activeFlowGen = -1;        // generation that owns sweep/replay/producer
        private bool _entrySnapshotOk;
        private double _entryGazeLength = double.NaN;
        // NEXT-NORMAL-ENTRY-01: frozen normal shared-entry preflight state (scalars only).
        // Set once in EnterVRCore after the pure-managed config validation; ReadyReached
        // re-reads the env and demands exact token equality BEFORE producer construction.
        private bool _normalSharedEntryFrozen;
        private string _normalSharedEntryToken;
        private int _normalSharedEntryFrozenGen = -1;
        private int _teardownGate;   // 0 free / 1 cleanup running (Interlocked) — no duplicate Stop/Destroy
        // P1 fix: when StopProducerIfAny returns FALSE the owner SDK transaction is not closed.
        // Native cleanup must NOT run early. A single retained deferred DispatcherTimer keeps
        // retrying only the owner StopAndRestore; on success it hands off to the SAME Stop->Destroy
        // gate (TeardownSession), exactly once. The (possibly pumping) tick uses a pure-managed
        // reentrancy guard so it can never re-enter itself.
        // P1 (2026-10-04): every retry timer is BOUND to the producer instance that armed it
        // (_producerRetrySource) and its tick re-verifies its own timer-instance identity, so a
        // late tick of a retired retry can never stop or clean a NEWER producer/flow. The two
        // honest native cleanup paths (Exit / TeardownSession) retire the pending retry
        // (timer + source fields) BEFORE their CAS gate.
        private System.Windows.Threading.DispatcherTimer _producerRetryTimer;
        private VCFrameProducer _producerRetrySource;   // producer instance the pending timer is armed for
        private int _producerRetryReentrancy;   // 0 idle / 1 inside tick (Interlocked, pure managed)
        // Host3B2A OBS-SHARED teardown coordinator (prepared only — card B wires every entry path).
        // _obsSharedStopSource latches the shared coordinator identity BEFORE the FIRST StopAndRestore;
        // the recorded native task + source identity are retained until the WHOLE stop verifies, so a
        // later Exit can never start the uncertain native cleanup a second time.
        private VCFrameProducer _obsSharedStopSource;
        private VCFrameProducer _obsSharedNativeSource;
        private Task _obsSharedNativeTask;
        private volatile bool _obsSharedNativeTaskReturned;
        private int _obsSharedLastNativeDestroyRc = int.MinValue;
        private int _obsSharedNativeDestroyState = int.MinValue;
        private bool _obsSharedTeardownCompleted;
        private int _obsSharedConfirmRetries;
        private long _obsSharedConfirmBeginQpc;
        private volatile bool _obsSharedHostTerminal;
        private Exception _obsSharedHostError;   // merged FULL original evidence (ToString kept)

        // ---------------- native C ABI (VRSIMNative.h) ----------------
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string fileName);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_SetLoaderPathW([MarshalAs(UnmanagedType.LPWStr)] string path);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_Create();

        [DllImport(NativeDllName)]
        private static extern int VRSIM_Start();

        [DllImport(NativeDllName)]
        private static extern int VRSIM_GetState();

        [StructLayout(LayoutKind.Sequential)]
        private struct VRSIMFrameState
        {
            public ulong frameSerial;
            public long displayTime;
            public long displayPeriod;
            public int shouldRender;
            public int trackingValid;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct VRSIMEyeView
        {
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public double[] posMm;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public double[] quatXyzw;
            public double fovLeft, fovRight, fovUp, fovDown;
        }

        [DllImport(NativeDllName)]
        private static extern int VRSIM_GetFrameState(out VRSIMFrameState fs);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_GetViews(out VRSIMEyeView left, out VRSIMEyeView right);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_Stop();

        [DllImport(NativeDllName)]
        private static extern int VRSIM_Destroy();

        [DllImport(NativeDllName)]
        private static extern IntPtr VRSIM_GetLastError();

        [DllImport(NativeDllName)]
        private static extern int VRSIM_GetDiagnostics(byte[] buf, int cap);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_SubmitStereoFrame(
            byte[] left, byte[] right, int width, int height, int stride,
            ulong frameSerial, ulong poseSerial, double cameraYawDeg);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_EnableSceneMode(int width, int height);

        [StructLayout(LayoutKind.Sequential)]
        internal struct VRSIMStereoStats
        {
            public ulong submitOk, submitFail, d3dUploads, vcLayerOk, reusedFrames;
            public ulong latestFrameSerial, quadOkCount;
            public int sceneMode, ringFree;
            // v2 (0.1.1.2)
            public ulong framesDropped, lastConsumedSerial;
            public int renderSourceMode;      // 0=SYNTHETIC_TEST 1=VC_SCENE
            public int illegalFallback;       // ILLEGAL_SYNTHETIC_FALLBACK
            public int requestedRenderSource; // what the plugin asked for
            // v3 (0.1.3.3) tracking evidence
            public ulong poseSerial, poseSamples, poseChanges;
            // v4 (0.1.3.3) pose/image association
            public ulong poseImageMismatch, lastLayerPoseSerial;
            // v5 (0.1.1.7) user presence
            public int userPresenceAvailable, userPresenceSupported, isUserPresent;
            // v6 (0.1.3.3) live-pipeline content evidence
            public double lastConsumedYawDeg;
            public ulong consumedYawChanges;
            // v7 (0.1.3.3)
            public ulong beginFrameFailed, d3dUploadCount;
        }

        [DllImport(NativeDllName)]
        private static extern int VRSIM_GetStereoStats(out VRSIMStereoStats stats);

        [StructLayout(LayoutKind.Sequential)]
        internal struct VRSIMBuildInfo
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string semanticVersion;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string buildTimestamp;
            public uint featureFlags;
        }

        [DllImport(NativeDllName)]
        private static extern int VRSIM_GetBuildInfo(out VRSIMBuildInfo info);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_DumpSceneTexture(int eye, byte[] buf, int cap, out int w, out int h);

        // 0.1.3.5 MONITOR: the single SubmitStereo entry caches the LAST SUCCESSFULLY
        // submitted stereo pair (validated deep copy) for the read-only monitor window.
        // The monitor never submits, never touches the camera, and adds no XR owner.
        // FIX .135b: validate inputs BEFORE the P/Invoke — native must never read an
        // out-of-range buffer; invalid calls return a negative host code, no cache.
        internal int SubmitStereo(byte[] l, byte[] r, int w, int h, int stride, ulong fs, ulong ps, double yawDeg = 0)
        {
            int code = CaptureProfile.ValidateInput(l, r, w, h, stride);
            if (code != 0) return code;
            int rc = VRSIM_SubmitStereoFrame(l, r, w, h, stride, fs, ps, yawDeg);
            if (rc == 0) StorePreview(l, r, w, h, stride, fs, ps);
            return rc;
        }

        private readonly object _previewLock = new object();
        private byte[] _prevLeft, _prevRight;
        private int _prevW, _prevH, _prevStride;
        private ulong _prevFs, _prevPs;
        private DateTime _prevUtc = DateTime.MinValue;
        private int _previewEpoch;            // bumped when a new flow clears the cache
        private ulong _previewStoreCount;     // successful StorePreview completions (content-fps basis)

        internal int PreviewEpochForMonitor() { lock (_previewLock) return _previewEpoch; }
        internal ulong PreviewStoreCountForMonitor() { lock (_previewLock) return _previewStoreCount; }

        private void StorePreview(byte[] l, byte[] r, int w, int h, int stride, ulong fs, ulong ps)
        {
            try
            {
                if (CaptureProfile.ValidateInput(l, r, w, h, stride) != 0) return;
                long needLong = (long)stride * (long)h;   // long math — no int overflow
                int need = (int)needLong;
                var lc = new byte[need]; var rc2 = new byte[need];
                Buffer.BlockCopy(l, 0, lc, 0, need);
                Buffer.BlockCopy(r, 0, rc2, 0, need);
                lock (_previewLock)
                {
                    _prevLeft = lc; _prevRight = rc2;
                    _prevW = w; _prevH = h; _prevStride = stride;
                    _prevFs = fs; _prevPs = ps; _prevUtc = DateTime.UtcNow;
                    _previewStoreCount++;
                }
            }
            catch (Exception ex) { Log("PREVIEW_STORE_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        internal bool TryGetPreview(out byte[] left, out byte[] right, out int w, out int h,
            out int stride, out ulong fs, out ulong ps, out DateTime utc, out int epoch)
        {
            lock (_previewLock)
            {
                left = _prevLeft; right = _prevRight; w = _prevW; h = _prevH; stride = _prevStride;
                fs = _prevFs; ps = _prevPs; utc = _prevUtc; epoch = _previewEpoch;
                return left != null && right != null;
            }
        }

        // ---------------- additive managed raw GPU bindings (V1 wire — NOT wired into the
        // producer yet; native-side epoch fields land with the followup native card) ----------------
        // Wire sizes: RawProjectionPlanV1 = 56 bytes, GpuPipelineStatsV1 = 120 bytes (Pack=8).
        // Host-side rejection codes for SubmitRawStereo: -30 serials/yaw, -31 plan
        // header/finite/sign/positive, -32 plan coverage/aspect, -33 float-conversion.
        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct RawProjectionPlanV1
        {
            public uint structBytes, version;
            public double tanL, tanR, tanU, tanD, tx, ty;
        }

        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        internal struct GpuPipelineStatsV1
        {
            public uint structBytes, version;
            public ulong rawAccepted, rawRejected, gpuPairCommitted, gpuPairFailed,
                exactPoseRejected, uniqueLiveLayerOk,
                lastSuccessfulLayerFrameSerial, lastSuccessfulLayerPoseSerial,
                activeFrameSerial, activePoseSerial;
            public uint activeSource, activeWidth, activeHeight, reserved;
            // reserved: bit0 = raw admission open; ALL other bits must stay zero.
            // activeSource: 0=none 1=CPU-live 2=GPU-live 3=static-preview
            public ulong lastGpuDispatchQpcTicks, sessionEpoch;
        }

        [DllImport(NativeDllName)]
        private static extern int VRSIM_GetGpuPipelineStatsV1(out GpuPipelineStatsV1 stats, uint capacity);

        [DllImport(NativeDllName)]
        private static extern int VRSIM_SubmitRawStereoV1(
            byte[] left, ulong leftBytes, byte[] right, ulong rightBytes,
            int width, int height, int stride,
            ulong frameSerial, ulong poseSerial, ulong sessionEpoch,
            double cameraYawDeg,
            ref RawProjectionPlanV1 leftPlan, ref RawProjectionPlanV1 rightPlan);

        /// <summary>Reads the V1 GPU-pipeline stats wire. Returns null on ANY failure —
        /// unsupported entry (EntryPointNotFound/DllNotFound), bad rc, wrong wire size or
        /// version. Never pretends success.</summary>
        internal GpuPipelineStatsV1? ReadGpuPipelineStats()
        {
            try
            {
                if (Marshal.SizeOf(typeof(GpuPipelineStatsV1)) != 120) return null;
                GpuPipelineStatsV1 st;
                int rc = VRSIM_GetGpuPipelineStatsV1(out st, 120);
                if (rc == 0 && st.structBytes == 120u && st.version == 1u) return st;
            }
            catch (EntryPointNotFoundException) { }
            catch (DllNotFoundException) { }
            return null;
        }

        /// <summary>API-support probe ONLY: old VRSIM_GetBuildInfo feature-flag bit1 plus a
        /// valid V1 stats wire. This proves the entry points exist and speak the wire —
        /// NOT actual content, performance or device proof. Safe before Create (lazy binding).</summary>
        internal bool RawGpuApiSupported()
        {
            try
            {
                VRSIMBuildInfo bi;
                if (VRSIM_GetBuildInfo(out bi) != 0) return false;
                if ((bi.featureFlags & 0x2u) == 0) return false;   // bit1 = raw GPU pipeline API
                return ReadGpuPipelineStats().HasValue;
            }
            catch (Exception ex)
            {
                Log("RAW_GPU_API_SUPPORT_FAIL " + ex.GetType().Name);
                return false;
            }
        }

        private static bool RawPlanFloatsUsable(RawProjectionPlanV1 p)
        {
            float fl = (float)p.tanL, fr = (float)p.tanR, fu = (float)p.tanU,
                  fd = (float)p.tanD, ftx = (float)p.tx, fty = (float)p.ty;
            if (float.IsNaN(fl) || float.IsInfinity(fl) || float.IsNaN(fr) || float.IsInfinity(fr)
                || float.IsNaN(fu) || float.IsInfinity(fu) || float.IsNaN(fd) || float.IsInfinity(fd)
                || float.IsNaN(ftx) || float.IsInfinity(ftx) || float.IsNaN(fty) || float.IsInfinity(fty))
                return false;
            // non-zero tangents + positive source half-tangents AFTER conversion
            return fl < 0f && fr > 0f && fd < 0f && fu > 0f && ftx > 0f && fty > 0f;
        }

        private static int ValidateRawProjectionPlanWire(RawProjectionPlanV1 p, int w, int h)
        {
            if (p.structBytes != 56u || p.version != 1u) return -31;
            if (!FiniteD(p.tanL) || !FiniteD(p.tanR) || !FiniteD(p.tanU) || !FiniteD(p.tanD)
                || !FiniteD(p.tx) || !FiniteD(p.ty)) return -31;
            if (!(p.tanL < 0 && p.tanR > 0 && p.tanD < 0 && p.tanU > 0 && p.tx > 0 && p.ty > 0)) return -31;
            // symmetric source box must cover the full asymmetric XR frustum
            if (p.tx + 1e-12 < Math.Max(-p.tanL, p.tanR)) return -32;
            if (p.ty + 1e-12 < Math.Max(p.tanU, -p.tanD)) return -32;
            // source aspect must match the submitted image aspect
            if (Math.Abs(p.tx / p.ty - (double)w / (double)h) > 1e-6) return -32;
            if (!RawPlanFloatsUsable(p)) return -33;
            return 0;
        }

        /// <summary>Additive mapping of an existing producer plan onto the V1 wire struct.
        /// Bool/string plan fields are deliberately NOT marshalled. Throws on an invalid
        /// plan — never emits a zeroed/fake wire row.</summary>
        internal static RawProjectionPlanV1 MakeRawProjectionPlan(VCFrameProducer.EyeProjectionPlan p)
        {
            if (!p.Valid)
                throw new InvalidOperationException("RAW_PLAN_REJECT reason=" +
                    (string.IsNullOrEmpty(p.RejectReason) ? "invalid" : p.RejectReason));
            var wire = new RawProjectionPlanV1();
            wire.structBytes = 56u;
            wire.version = 1u;
            wire.tanL = p.TanL; wire.tanR = p.TanR;
            wire.tanU = p.TanU; wire.tanD = p.TanD;
            wire.tx = p.Tx; wire.ty = p.Ty;
            return wire;
        }

        /// <summary>Additive raw-GPU submit. ALL validation runs BEFORE the P/Invoke so
        /// native can never over-read a buffer. Returns the actual native rc; negative
        /// host codes (-30..-33) mean the call never reached native.</summary>
        internal int SubmitRawStereo(byte[] l, byte[] r, int w, int h, int stride,
            ulong fs, ulong ps, ulong epoch,
            RawProjectionPlanV1 lp, RawProjectionPlanV1 rp, double yawDeg = 0)
        {
            int code = CaptureProfile.ValidateInput(l, r, w, h, stride);
            if (code != 0) return code;
            if (fs == 0UL || ps == 0UL || epoch == 0UL || !FiniteD(yawDeg)) return -30;
            code = ValidateRawProjectionPlanWire(lp, w, h);
            if (code != 0) return code;
            code = ValidateRawProjectionPlanWire(rp, w, h);
            if (code != 0) return code;
            int rc = VRSIM_SubmitRawStereoV1(l, (ulong)l.LongLength, r, (ulong)r.LongLength,
                w, h, stride, fs, ps, epoch, yawDeg, ref lp, ref rp);
            if (rc == 0) ClearPreviewForRawEnqueue();
            return rc;
        }

        // Raw GPU submissions carry SYMMETRIC SOURCE captures, not XR-projected pixels —
        // they must never enter the monitor preview cache. The GPU-projected runtime/dump
        // output is the actual headset source; a future monitor card will fetch that
        // properly bound output. So on a successful raw enqueue we only CLEAR any cached
        // preview (producer state, _previewEpoch and _previewStoreCount untouched); the
        // CPU SubmitStereo preview path is unchanged.
        private void ClearPreviewForRawEnqueue()
        {
            lock (_previewLock)
            {
                if (_prevLeft == null && _prevRight == null) return;
                _prevLeft = null; _prevRight = null;
                _prevW = _prevH = _prevStride = 0;
                _prevFs = _prevPs = 0; _prevUtc = DateTime.MinValue;
            }
            Log("PREVIEW_CLEARED raw_gpu_enqueue");
        }

        // ---------------- monitor read-only ABI wrappers (existing exports only) ----------------
        internal int NativeStateForMonitor()
        { try { return VRSIM_GetState(); } catch { return -1; } }

        internal string NativeErrorForMonitor()
        { return NativeError(); }

        internal string NativeVersionInfo()
        {
            try { VRSIMBuildInfo bi; if (VRSIM_GetBuildInfo(out bi) == 0) return bi.semanticVersion + " built " + bi.buildTimestamp; }
            catch (Exception ex) { return "read_fail:" + ex.GetType().Name; }
            return "n/a";
        }

        internal bool TryGetFrameTiming(out ulong frameSerial, out long displayPeriodNs,
            out int shouldRender, out int trackingValid)
        {
            frameSerial = 0; displayPeriodNs = 0; shouldRender = 0; trackingValid = 0;
            try
            {
                VRSIMFrameState fs2;
                if (VRSIM_GetFrameState(out fs2) == 0)
                { frameSerial = fs2.frameSerial; displayPeriodNs = fs2.displayPeriod; shouldRender = fs2.shouldRender; trackingValid = fs2.trackingValid; return true; }
            }
            catch { }
            return false;
        }

        /// <summary>Honest phase for the monitor — XR state 7/8 can NEVER show "live".</summary>
        internal string MonitorPhase()
        {
            int st = NativeStateForMonitor();
            if (st == 8) return "failure";
            if (st == 7) return "stopped";
            if (_creating || _readyWaiting) return "starting";
            if (_sweep != null) return "diagnostic";
            var p = _producer;
            if (p != null) return _diagnosticFlow ? "diagnostic-live" : p.CurrentPhase;
            if (st == 5 || st == 6) return "starting";
            return "stopped";
        }

        internal bool DiagnosticFlowActive { get { return _diagnosticFlow; } }
        internal bool VrActiveForMonitor { get { return _vrActive; } }
        internal static string PluginVersionForMonitor { get { return PluginVersion; } }

        internal string ProducerMonitorInfo()
        {
            var p = _producer;
            if (p == null) return "n/a";
            return "fovSkips=" + p.EyeFovSkipsForMonitor + " invalidTracking=" + p.InvalidTrackingFramesForMonitor;
        }

        // ---------------- SHARED-GPU MONITOR SAMPLE (read-only snapshot card) ----------------
        // Immutable numeric-only monitor sample for the shared-GPU route. Holds managed scalars
        // and numeric wire copies (Stats120 / Stop32 / Stats72) ONLY — never an SDK view, texture,
        // native handle/pointer, and never pixels. The producer reference is an opaque identity
        // used solely for ReferenceEquals rechecks; no SDK member is read through it afterwards.
        // This adds NO lifecycle/timer/preview change: the old CPU monitor wrappers are untouched.
        internal sealed class SharedGpuMonitorSample
        {
            public readonly bool Valid;               // false = read/shape/identity failure: show invalid/failed, never green
            public readonly bool ReadError;           // exception / native rc / wire-shape failure during the bounded read group
            public readonly string Reason;            // machine-readable classification, never null
            public readonly string LiveReject;        // bound-valid but live-gate rejection reason (null = live or not evaluated)

            public readonly object ProducerIdentity;  // opaque ReferenceEquals key only
            public readonly int MainGeneration;       // _generation frozen at sample
            public readonly int ActiveFlowGeneration; // _activeFlowGen frozen at sample
            public readonly int OriginalHostedStartGeneration; // producer.HostedStartGeneration at freeze (-1 = none)

            public readonly bool SharedSelected;      // false = CPU monitor branch continues unchanged
            public readonly bool SharedPreparing;     // frozen normal shared ENTRY evidence only — no bound/content claim
            public readonly bool BoundValid;          // session/bind identity proven from actual Stop32/Stats72/Stats120
            public readonly bool LiveValid;           // full live gate passed (see CaptureSharedGpuMonitorSample)

            public readonly string Phase;             // frozen producer CurrentPhase ("preparing" = entry evidence only)
            public readonly string Failure;           // recorded backend/hosted failure strings ("" = none)
            public readonly long SampleQpc;           // Stopwatch.GetTimestamp(): the monitor OBSERVATION instant only.
                                                      // lastGpuDispatchQpcTicks is a duration, never a timestamp — no frame-age math here.

            public readonly uint CompiledRequestedEdge;     // SharedGpuTicketEdgeV1 (compiled; NO env re-read)
            public readonly int CompiledFrameWidth;         // VCFrameProducer.FrameW at sample (compiled constant)
            public readonly int CompiledFrameHeight;        // VCFrameProducer.FrameH at sample

            public readonly GpuPipelineStatsV1? StatsA;     // numeric copies of the bracketing reads
            public readonly GpuPipelineStatsV1? StatsB;     // THE coherent content counter/metadata snapshot for future rate math
            public readonly SharedGpuStopStateV1? Stop;     // actual Stop32 copy
            public readonly SharedGpuBridgeStatsV1? Bridge; // actual Stats72 copy
            public readonly int StopRc;
            public readonly int BridgeRc;

            public readonly int? ActualWidth;         // filled ONLY from a validated StatsB — never substituted with the requested edge
            public readonly int? ActualHeight;

            public readonly int ActualNativeState;    // VRSIM_GetState result at sample (-1 = unread)
            public readonly bool HasFrameTiming;
            public readonly ulong TimingFrameSerial;
            public readonly long DisplayPeriodNs;
            public readonly int TrackingValid;
            public readonly int ShouldRender;

            private SharedGpuMonitorSample(Draft d)
            {
                Valid = d.Valid; ReadError = d.ReadError; Reason = d.Reason ?? ""; LiveReject = d.LiveReject;
                ProducerIdentity = d.ProducerIdentity;
                MainGeneration = d.MainGeneration; ActiveFlowGeneration = d.ActiveFlowGeneration;
                OriginalHostedStartGeneration = d.OriginalHostedStartGeneration;
                SharedSelected = d.SharedSelected; SharedPreparing = d.SharedPreparing;
                BoundValid = d.BoundValid; LiveValid = d.LiveValid;
                Phase = d.Phase ?? ""; Failure = d.Failure ?? ""; SampleQpc = d.SampleQpc;
                CompiledRequestedEdge = d.CompiledRequestedEdge;
                CompiledFrameWidth = d.CompiledFrameWidth; CompiledFrameHeight = d.CompiledFrameHeight;
                StatsA = d.StatsA; StatsB = d.StatsB; Stop = d.Stop; Bridge = d.Bridge;
                StopRc = d.StopRc; BridgeRc = d.BridgeRc;
                ActualWidth = d.ActualWidth; ActualHeight = d.ActualHeight;
                ActualNativeState = d.ActualNativeState;
                HasFrameTiming = d.HasFrameTiming; TimingFrameSerial = d.TimingFrameSerial;
                DisplayPeriodNs = d.DisplayPeriodNs;
                TrackingValid = d.TrackingValid; ShouldRender = d.ShouldRender;
            }

            // Mutable construction form; never escapes the sampling helper — callers only ever
            // see the sealed immutable sample above.
            internal struct Draft
            {
                public bool Valid, ReadError;
                public string Reason, LiveReject;
                public object ProducerIdentity;
                public int MainGeneration, ActiveFlowGeneration, OriginalHostedStartGeneration;
                public bool SharedSelected, SharedPreparing, BoundValid, LiveValid;
                public string Phase, Failure;
                public long SampleQpc;
                public uint CompiledRequestedEdge;
                public int CompiledFrameWidth, CompiledFrameHeight;
                public GpuPipelineStatsV1? StatsA, StatsB;
                public SharedGpuStopStateV1? Stop;
                public SharedGpuBridgeStatsV1? Bridge;
                public int StopRc, BridgeRc;
                public int? ActualWidth, ActualHeight;
                public int ActualNativeState;
                public bool HasFrameTiming;
                public ulong TimingFrameSerial;
                public long DisplayPeriodNs;
                public int TrackingValid, ShouldRender;

                public SharedGpuMonitorSample Seal() { return new SharedGpuMonitorSample(this); }
            }
        }

        /// <summary>Read-only shared-GPU monitor sample. Call ONLY when _disp.CheckAccess() is
        /// true (the existing monitor timer path); a null dispatcher or an off-dispatcher call
        /// returns an invalid sample WITHOUT touching native. Before a producer exists, the frozen
        /// normal shared-entry flag is reported as selection/"preparing" evidence only — no lazy
        /// API-support probe, no bound/dimension/count claim, no env read, and a non-shared
        /// situation returns SharedSelected=false so the old CPU monitor branch continues.
        /// For a current shared producer exactly ONE bounded read group runs:
        /// Stats120 A → actual Stop32 → actual Stats72 → Stats120 B, then ONE NativeState and ONE
        /// frame-timing read. No loop, retry, wait, lifecycle call or GPU operation. The frozen
        /// managed identity is then rechecked; any exception, rc, shape or identity change yields
        /// Valid=false (never green). NOTE: the bracketing proves session-identity stability only —
        /// these independent native getters are NOT atomically synchronized as a whole; counters
        /// may legitimately advance between A and B, so StatsB alone is the content snapshot.</summary>
        internal SharedGpuMonitorSample CaptureSharedGpuMonitorSample()
        {
            var d = new SharedGpuMonitorSample.Draft();
            d.CompiledRequestedEdge = SharedGpuTicketEdgeV1;
            d.CompiledFrameWidth = VCFrameProducer.FrameW;
            d.CompiledFrameHeight = VCFrameProducer.FrameH;
            d.OriginalHostedStartGeneration = -1;
            d.ActualNativeState = -1;
            d.StopRc = int.MinValue;
            d.BridgeRc = int.MinValue;

            var disp = _disp;
            if (disp == null) { d.Reason = "dispatcher_null"; return d.Seal(); }
            if (!disp.CheckAccess()) { d.Reason = "off_dispatcher"; return d.Seal(); }

            var p = _producer;
            d.ProducerIdentity = p;
            d.MainGeneration = _generation;
            d.ActiveFlowGeneration = _activeFlowGen;
            d.SampleQpc = Stopwatch.GetTimestamp();

            // ---- no producer yet: selection evidence only, ZERO native calls ----
            if (p == null)
            {
                bool preparing = _normalSharedEntryFrozen && _normalSharedEntryFrozenGen == _generation;
                if (preparing)
                {
                    bool stillStarting = _busy || _creating || _readyWaiting;
                    d.Valid = true; d.SharedSelected = true;
                    d.SharedPreparing = stillStarting;
                    d.Phase = stillStarting ? "preparing" : "stopped";
                    d.Reason = stillStarting ? "shared_entry_preparing_selection_evidence_only"
                                             : "shared_entry_inactive_selection_evidence_only";
                }
                else
                {
                    d.Valid = true; d.SharedSelected = false;
                    d.Reason = "no_producer_no_shared_selection";
                }
                return d.Seal();
            }

            // ---- freeze the pure-managed source state (no SDK/native work in these getters) ----
            bool selected, admission, ready;
            string phase, failBackend, failHosted;
            int startGen;
            try
            {
                selected = p.ObsSharedSelected;
                startGen = p.HostedStartGeneration;
                phase = p.CurrentPhase;
                admission = p.CaptureAdmissionOpen;
                ready = p.IsCaptureBackendReady;
                failBackend = p.CaptureBackendFailureReason ?? string.Empty;
                failHosted = p.HostedCaptureFailureReasonForMonitor ?? string.Empty;
            }
            catch (Exception ex) { d.ReadError = true; d.Reason = "managed_state_read_fail:" + ex.GetType().Name; return d.Seal(); }

            d.Phase = phase;
            d.OriginalHostedStartGeneration = startGen;
            d.Failure = failBackend.Length == 0 ? failHosted
                        : (failHosted.Length == 0 ? failBackend : failBackend + ";" + failHosted);

            if (!selected)
            {
                d.Valid = true; d.SharedSelected = false;
                d.Reason = "producer_not_shared_cpu_branch_continues";
                return d.Seal();
            }
            d.SharedSelected = true;

            // old retained producers must never present stale-flow evidence as live
            if (d.ActiveFlowGeneration <= 0 || d.ActiveFlowGeneration != d.MainGeneration)
            {
                d.Reason = "main_flow_not_current";
                return d.Seal();
            }

            // ---- one bounded read group: Stats120 A → Stop32 → Stats72 → Stats120 B, then
            // one NativeState + one frame-timing read. Missing-export exceptions become a
            // honestly invalid sample — never a fallback and never a green claim. ----
            GpuPipelineStatsV1? sa = null, sb = null;
            SharedGpuStopStateV1 stop = default(SharedGpuStopStateV1);
            SharedGpuBridgeStatsV1 bridge = default(SharedGpuBridgeStatsV1);
            int stopRc, bridgeRc;
            int nativeState = -1;
            bool timingOk = false;
            ulong timingFs = 0; long displayPeriodNs = 0; int shouldRender = 0, trackingValid = 0;
            try
            {
                sa = ReadGpuPipelineStats();
                stopRc = ReadSharedGpuStop(out stop);
                bridgeRc = ReadSharedGpuBridgeStats(out bridge);
                sb = ReadGpuPipelineStats();
                nativeState = NativeStateForMonitor();
                timingOk = TryGetFrameTiming(out timingFs, out displayPeriodNs, out shouldRender, out trackingValid);
            }
            catch (Exception ex)
            {
                d.ReadError = true;
                d.Reason = "read_group_exception:" + ex.GetType().Name;
                return d.Seal();
            }
            d.StopRc = stopRc; d.BridgeRc = bridgeRc;
            d.ActualNativeState = nativeState;
            d.HasFrameTiming = timingOk;
            d.TimingFrameSerial = timingFs;
            d.DisplayPeriodNs = displayPeriodNs;
            d.TrackingValid = trackingValid;
            d.ShouldRender = shouldRender;

            // ---- frozen identity/state recheck after the read group ----
            bool recheckOk;
            try
            {
                recheckOk = ReferenceEquals(_producer, p)
                    && _generation == d.MainGeneration
                    && _activeFlowGen == d.ActiveFlowGeneration
                    && p.HostedStartGeneration == startGen
                    && p.ObsSharedSelected
                    && p.CurrentPhase == phase
                    && p.CaptureAdmissionOpen == admission
                    && p.IsCaptureBackendReady == ready
                    && (p.CaptureBackendFailureReason ?? string.Empty) == failBackend
                    && (p.HostedCaptureFailureReasonForMonitor ?? string.Empty) == failHosted;
            }
            catch (Exception ex)
            {
                d.ReadError = true;
                d.Reason = "recheck_exception:" + ex.GetType().Name;
                return d.Seal();
            }
            if (!recheckOk) { d.Reason = "identity_or_state_recheck_changed"; return d.Seal(); }

            d.SampleQpc = Stopwatch.GetTimestamp();

            // ---- wire shape / session validation ----
            if (!sa.HasValue || !sb.HasValue)
            {
                d.ReadError = true;
                d.Reason = "stats120_unavailable";
                return d.Seal();
            }
            var a = sa.Value;
            var b = sb.Value;
            if (a.structBytes != 120u || a.version != 1u || b.structBytes != 120u || b.version != 1u)
            {
                d.ReadError = true; d.Reason = "stats120_shape_invalid"; return d.Seal();
            }
            if (a.sessionEpoch != b.sessionEpoch || b.sessionEpoch == 0ul)
            {
                d.Reason = "epoch_unstable_or_zero"; return d.Seal();
            }
            if ((a.reserved & ~1u) != 0u || (b.reserved & ~1u) != 0u)
            {
                d.ReadError = true; d.Reason = "stats_reserved_unknown_bits"; return d.Seal();
            }
            if (stopRc != 0 || stop.structBytes != SharedGpuStopBytesV1 || stop.version != SharedGpuWireVersionV1
                || stop.reserved != 0u || (stop.flags & ~15u) != 0u)
            {
                d.ReadError = true; d.Reason = "stop32_rc_or_shape_invalid:" + stopRc; return d.Seal();
            }
            if (bridgeRc != 0 || bridge.version != SharedGpuWireVersionV1 || bridge.structBytes != SharedGpuStatsBytesV1
                || bridge.reservedPadding != 0u
                || bridge.slotState0 > 5u || bridge.slotState1 > 5u || bridge.slotState2 > 5u)
            {
                d.ReadError = true; d.Reason = "stats72_rc_shape_or_slot_invalid:" + bridgeRc; return d.Seal();
            }

            d.StatsA = a; d.StatsB = b; d.Stop = stop; d.Bridge = bridge;

            // ---- bind identity: same epoch, and the bound generation must be the exact
            // current producer's original HostedStartGeneration ----
            if (stop.boundEpoch != b.sessionEpoch || stop.boundEpoch == 0ul)
            {
                d.Valid = true; d.BoundValid = false;
                d.Reason = "shared_bind_epoch_not_matching_stats"; return d.Seal();
            }
            if (startGen <= 0 || stop.boundGeneration == 0u || stop.boundGeneration != (uint)startGen)
            {
                d.Valid = true; d.BoundValid = false;
                d.Reason = "shared_bind_generation_mismatch"; return d.Seal();
            }
            if (bridge.initCount == 0ul)
            {
                d.Valid = true; d.BoundValid = false;
                d.Reason = "bridge_not_initialized"; return d.Seal();
            }
            if (bridge.quarantineCount != 0ul)
            {
                d.Valid = true; d.BoundValid = false;
                d.Reason = "bridge_quarantined"; return d.Seal();
            }

            d.Valid = true;
            d.BoundValid = true;
            // Observed dimensions come ONLY from validated StatsB GPU-live metadata that
            // matches the compiled shared edge exactly — never a static/none source, never a default.
            if (b.activeSource == 2u
                && b.activeFrameSerial != 0ul && b.activePoseSerial != 0ul
                && b.activeWidth == SharedGpuTicketEdgeV1 && b.activeHeight == SharedGpuTicketEdgeV1
                && b.activeWidth == (uint)d.CompiledFrameWidth && b.activeHeight == (uint)d.CompiledFrameHeight)
            {
                d.ActualWidth = (int)b.activeWidth;
                d.ActualHeight = (int)b.activeHeight;
            }

            // ---- live qualification (all frozen evidence, no defaults) ----
            string liveReject = null;
            if (!admission) liveReject = "admission_closed";
            else if (!ready) liveReject = "source_not_ready";
            else if (d.Failure.Length != 0) liveReject = "failure_recorded";
            else if (phase != "live") liveReject = "phase_not_live";
            else if (nativeState != 5 && nativeState != 6) liveReject = "native_state_not_visible_or_focused";
            else if (b.reserved != 1u) liveReject = "raw_admission_bit_not_open";
            else if (stop.flags != 0u) liveReject = "stop_flags_nonzero";
            else if (b.activeSource != 2u) liveReject = "active_source_not_gpu_live";
            else if (b.activeFrameSerial == 0ul || b.activePoseSerial == 0ul
                  || b.lastSuccessfulLayerFrameSerial == 0ul || b.lastSuccessfulLayerPoseSerial == 0ul)
                liveReject = "no_active_or_last_success_frame_pose";
            else if (b.activeWidth != SharedGpuTicketEdgeV1 || b.activeHeight != SharedGpuTicketEdgeV1
                  || b.activeWidth != (uint)d.CompiledFrameWidth || b.activeHeight != (uint)d.CompiledFrameHeight)
                liveReject = "active_dims_not_compiled_shared_edge";
            else if (!timingOk) liveReject = "frame_timing_unavailable";
            else if (trackingValid == 0 || shouldRender == 0) liveReject = "tracking_or_should_render_invalid";
            if (liveReject == null) d.LiveValid = true;
            else d.LiveReject = liveReject;
            d.Reason = d.LiveValid ? "shared_live_valid" : "shared_bound_not_live";
            return d.Seal();
        }

        internal byte[] DumpSceneTextureBytes(int eye)
        {
            try
            {
                var buf = new byte[VCFrameProducer.FrameW * VCFrameProducer.FrameH * 4];
                int w, h;
                if (VRSIM_DumpSceneTexture(eye, buf, buf.Length, out w, out h) == 0)
                { VCFrameProducer.ForceAlpha(buf); return buf; }
            }
            catch { }
            return null;
        }

        internal void SweepFinished(bool pass, string json)
        {
            try { File.WriteAllText(Path.Combine(_logDir, "dynamic-pipeline-validation.json"), json); } catch { }
            Log("SWEEP_RESULT pass=" + pass);
            if (_activeFlowGen != _generation)
            {
                Log("SWEEP_FLOW_STALE flow=" + _activeFlowGen + " cur=" + _generation + " — session was exited/re-entered; sweep result ignored");
                _sweep = null;
                return;
            }
            if (pass && _sweep != null)
            {
                Msg("[VRSIM] 相机扫摆动态链 20s 全部通过 — 开始姿态重放测试…");
                _sweep.RunReplay();   // 0.1.3.3: the call was missing in 0.1.1.8 (replay never ran)
                return;               // _sweep nulled in ReplayFinished
            }
            if (_sweep != null) Msg("[VRSIM] 相机扫摆动态链 FAIL — VR 保持静态，详见 sweep 证据");
            _sweep = null;
        }

        internal void ReplayFinished(bool pass, string json)
        {
            try { File.WriteAllText(Path.Combine(_logDir, "tracking-replay-validation.json"), json); } catch { }
            _sweep = null;
            Log("REPLAY_RESULT pass=" + pass);
            if (_activeFlowGen != _generation)
            {
                Log("REPLAY_FLOW_STALE flow=" + _activeFlowGen + " cur=" + _generation + " — session was exited/re-entered; producer NOT started");
                return;
            }
            if (!_diagnosticFlow)
            {
                // the normal entry never runs sweep/replay — never label it LIVE_PIPELINE_PASS
                Log("REPLAY_IGNORED_NOT_DIAGNOSTIC — normal entry does not run the validation chain");
                return;
            }
            if (pass)
            {
                Msg("[VRSIM] 姿态重放通过 — LIVE PIPELINE 全链机器验证完成（仅诊断流，正常入口不依赖此门）");
                // 0.1.3.3: pipeline proven — re-enable live head tracking on top of it.
                // EntryCalibration locks the origin at FOCUSED (worn) + 1.5 s stability.
                bool producerStarted = false;
                _disp.Invoke((System.Action)(() =>
                {
                    if (_producer != null)
                    {
                        Log("DIAGNOSTIC_FLOW_PRODUCER_REFUSED existing producer retained (pending cleanup) — not overwriting");
                        return;
                    }
                    _producer = new VCFrameProducer(this, _disp);
                    // MUST be the Enter-time snapshot — never re-adopt the post-sweep camera
                    _producer.CaptureEntryState(_entryViewForSweep, _entryFovForSweep, _entryGazeLength);
                    _producer.Start();
                    producerStarted = true;
                    Log("DIAGNOSTIC_FLOW_PRODUCER_START (diagnostic sweep/replay passed)");
                }));
                if (producerStarted) Msg("[VRSIM] Waiting for headset…");
                else Msg("[VRSIM] 帧生产者清理失败被保留 — 诊断生产者未启动，请点 Exit VR 重试清理");
            }
            else Msg("[VRSIM] 姿态重放 FAIL — 详见 replay 证据");
        }

        private CameraSweepController _sweep;
        private Matrix _entryViewForSweep;
        private double _entryFovForSweep = 35.0;

        internal void WriteEntryState(Matrix entryView, double fovY)
        {
            try
            {
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                // entryView is WORLD-TO-CAMERA (pixel-verified): its raw .P is the view
                // TRANSLATION, not the camera location. True camera world position = Inverse().P.
                Matrix cam = entryView.Inverse();
                string j = "{\"vcEntryState\":{\"pxMm\":" + cam.Px.ToString("0.000", ci)
                    + ",\"pyMm\":" + cam.Py.ToString("0.000", ci)
                    + ",\"pzMm\":" + cam.Pz.ToString("0.000", ci)
                    + ",\"matrixConvention\":\"world-to-camera\""
                    + ",\"rawViewTranslation\":[" + entryView.Px.ToString("0.000", ci) + "," + entryView.Py.ToString("0.000", ci) + "," + entryView.Pz.ToString("0.000", ci) + "]"
                    + ",\"fovYDeg\":" + fovY.ToString("0.00", ci)
                    + ",\"capturedUtc\":\"" + DateTime.UtcNow.ToString("o") + "\"}}";
                File.WriteAllText(Path.Combine(_logDir, "vc-entry-state.json"), j);
            }
            catch { }
        }

        internal bool TryGetViews(out VRSIMEyeView left, out VRSIMEyeView right)
        { return VRSIM_GetViews(out left, out right) == 0; }

        // Stable-serial bracket: stats -> GetViews -> stats. Not atomic; existing ABI only, no new native export.
        internal bool TryGetViewsWithSerial(out VRSIMEyeView left, out VRSIMEyeView right, out ulong poseSerial)
        {
            left = default(VRSIMEyeView);
            right = default(VRSIMEyeView);
            poseSerial = 0;
            try
            {
                for (int attempt = 0; attempt < 3; attempt++)
                {
                    var before = ReadStereoStats();
                    VRSIMEyeView l, r;
                    int rc = VRSIM_GetViews(out l, out r);
                    var after = ReadStereoStats();
                    if (rc == 0 && before.HasValue && after.HasValue
                        && before.Value.poseSerial != 0
                        && before.Value.poseSerial == after.Value.poseSerial)
                    {
                        left = l; right = r; poseSerial = before.Value.poseSerial;
                        return true;
                    }
                }
                return false;
            }
            catch
            {
                left = default(VRSIMEyeView);
                right = default(VRSIMEyeView);
                poseSerial = 0;
                return false;
            }
        }

        private static string Unquote() { return "\"pass\":true"; }

        private readonly System.Collections.Generic.HashSet<string> _msgOnce = new System.Collections.Generic.HashSet<string>();
        internal void MsgOnce(string text, string key)
        { if (_msgOnce.Add(key)) Msg(text); }

        internal bool IsSessionFocused()
        { return VRSIM_GetState() == 6; }

        // existing ABI only (VRSIM_GetFrameState) — no new native exports (review item 4)
        internal bool IsTrackingValid()
        {
            try { VRSIMFrameState fs; return VRSIM_GetFrameState(out fs) == 0 && fs.trackingValid != 0; }
            catch { return false; }
        }

        internal ulong LatestPoseSerial()
        { var st = ReadStereoStats(); return st.HasValue ? st.Value.poseSerial : 0; }

        internal void WriteVrEntryOrigin(double[] center, double[] q)
        {
            try
            {
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                string j = "{\"vrEntryOrigin\":{\"headCenterMm\":[" + center[0].ToString("0.000", ci) + "," + center[1].ToString("0.000", ci) + "," + center[2].ToString("0.000", ci) + "],\"qXyzw\":[" + q[0].ToString("0.000000", ci) + "," + q[1].ToString("0.000000", ci) + "," + q[2].ToString("0.000000", ci) + "," + q[3].ToString("0.000000", ci) + "],\"note\":\"head delta relative to THIS pose maps onto VC_ENTRY_CAMERA (CARD16 quantities)\"}}";
                File.WriteAllText(Path.Combine(_logDir, "vr-entry-origin.json"), j);
            }
            catch { }
        }

        internal VRSIMStereoStats? ReadStereoStats()
        {
            try { VRSIMStereoStats st; if (VRSIM_GetStereoStats(out st) == 0) return st; } catch { }
            return null;
        }

        // PHASE 1 (02A-FIX): prove WHICH binaries the running process actually loaded.
        private void WriteLoadedBinaries()
        {
            try
            {
                var asm = typeof(VRSIMPlugin).Assembly.Location;
                string nativePath = null, nativeVersion = null, nativeTs = null; uint flags = 0; string nativeSemVer = null;
                try
                {
                    using (var p = Process.GetCurrentProcess())
                        foreach (ProcessModule m in p.Modules)
                            if (string.Equals(m.ModuleName, NativeDllName, StringComparison.OrdinalIgnoreCase))
                            { nativePath = m.FileName; nativeVersion = m.FileVersionInfo.FileVersion; break; }
                    VRSIMBuildInfo bi;
                    if (VRSIM_GetBuildInfo(out bi) == 0)
                    { nativeSemVer = bi.semanticVersion; nativeTs = bi.buildTimestamp; flags = bi.featureFlags; }
                }
                catch { }
                string sha(string path)
                {
                    if (path == null || !File.Exists(path)) return null;
                    using (var sha1 = System.Security.Cryptography.SHA256.Create())
                        return BitConverter.ToString(sha1.ComputeHash(File.ReadAllBytes(path))).Replace("-", "").ToLowerInvariant();
                }
                var name = new System.Reflection.AssemblyName(GetType().Assembly.FullName);
                var j = "{"
                    + "\"loadedPlugin\":{\"path\":\"" + EscJ(asm) + "\",\"assemblyVersion\":\"" + name.Version + "\"," 
                    + "\"fileVersion\":\"" + GetType().Assembly.GetName().Version + "\",\"sha256\":\"" + (sha(asm) ?? "n/a") + "\"},"
                    + "\"loadedNative\":{\"path\":\"" + EscJ(nativePath ?? "NOT_LOADED") + "\",\"fileVersion\":\"" + (nativeVersion ?? "n/a") + "\","
                    + "\"semanticVersion\":\"" + (nativeSemVer ?? "n/a") + "\",\"buildTimestamp\":\"" + (nativeTs ?? "n/a").Replace("\\", "/") + "\","
                    + "\"featureFlags\":" + flags + ",\"sha256\":\"" + (sha(nativePath) ?? "n/a") + "\"},"
                    + "\"expectedVersion\":\"" + PluginVersion + "\"}";
                File.WriteAllText(Path.Combine(_logDir, "loaded-binaries.json"), j);
                Log("LOADED_BINARIES plugin=" + name.Version + " native=" + (nativeSemVer ?? "?") + " flags=" + flags);
            }
            catch (Exception ex) { Log("LOADED_BINARIES_FAIL " + ex.Message); }
        }

        private static string EscJ(string s2)
        { return s2 == null ? "" : s2.Replace("\\", "/").Replace("\"", "'"); }

        // PHASE 17 (02A-FIX): read back the exact texture the OpenXR layer references.
        internal bool DumpSceneTexturePng(int eye, string path)
        {
            try
            {
                int w, h;
                var buf = new byte[VCFrameProducer.FrameW * VCFrameProducer.FrameH * 4];
                int rc = VRSIM_DumpSceneTexture(eye, buf, buf.Length, out w, out h);
                if (rc != 0) { Log("TEXTURE_QA_FAIL eye=" + eye + " rc=" + rc); return false; }
                var bs = System.Windows.Media.Imaging.BitmapSource.Create(w, h, 96, 96,
                    System.Windows.Media.PixelFormats.Bgra32, null, buf, w * 4);
                var enc = new System.Windows.Media.Imaging.PngBitmapEncoder();
                enc.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bs));
                using (var fs = File.Create(path)) enc.Save(fs);
                Log("TEXTURE_QA_SAVED eye=" + eye + " " + w + "x" + h + " -> " + path);
                return true;
            }
            catch (Exception ex) { Log("TEXTURE_QA_FAIL " + ex.GetType().Name + ": " + ex.Message); return false; }
        }

        private static string NativeError()
        {
            try { return Marshal.PtrToStringAnsi(VRSIM_GetLastError()) ?? ""; }
            catch { return ""; }
        }

        // ---------------- lifecycle ----------------
        public void Initialize()
        {
            if (_initialized) return;
            _initialized = true;
            _disp = System.Windows.Threading.Dispatcher.CurrentDispatcher;

            _logDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "KUKA", "iiQWorks.Sim", "VRSIM");
            try { Directory.CreateDirectory(_logDir); } catch { }
            _logPath = Path.Combine(_logDir, "vr-plugin.log");
            _diagPath = Path.Combine(_logDir, "diagnostics.json");
            _nativeDir = Path.GetDirectoryName(typeof(VRSIMPlugin).Assembly.Location);
            Log("PLUGIN_INIT v" + PluginVersion + " nativeDir=" + (_nativeDir ?? "null"));
            Log(CaptureProfile.DiagnosticText);

            // Tab + group + two buttons (proven RegisterSite/ActionItem recipe)
            try
            {
                Ux.RegisterSite(new UXSiteSetup { UXSiteIdPath = "VRSIMTab", UXSiteCaption = "VRSIM", UXSiteType = UXSiteType.Tab });
                Ux.RegisterSite(new UXSiteSetup { UXSiteIdPath = "VRSIMTab/VR", UXSiteCaption = "VR Mode", UXSiteType = UXSiteType.RibbonGroup });
                _enter = new ActionItem("VRSIM.EnterVR", "Enter VR", "Program/rTeach", (System.Action)EnterVR,
                    MenuTools.ButtonTool, false, false)
                {
                    ContextFilter = Contexts.All, RibbonId = "VRSIM.EnterVR",
                    ToolSize = ToolSizingMode.ImageAndTextNormal,
                    ToolTipHeader = "Enter VR", ToolTipContent = "启动进程内 OpenXR 会话"
                };
                Commands.RegisterActionItem(_enter, new UXSiteSetup { UXSiteIdPath = "VRSIMTab/VR", EntryId = _enter.Id, EntryCaption = "Enter VR" });
                _exit = new ActionItem("VRSIM.ExitVR", "Exit VR", "Program/rTeach", (System.Action)ExitVR,
                    MenuTools.ButtonTool, false, false)
                {
                    ContextFilter = Contexts.All, RibbonId = "VRSIM.ExitVR",
                    ToolSize = ToolSizingMode.ImageAndTextNormal,
                    ToolTipHeader = "Exit VR", ToolTipContent = "结束 VR 会话并释放资源"
                };
                Commands.RegisterActionItem(_exit, new UXSiteSetup { UXSiteIdPath = "VRSIMTab/VR", EntryId = _exit.Id, EntryCaption = "Exit VR" });
                // 0.1.3.5: explicit diagnostic sweep (strict chain kept, never automatic on
                // normal Enter) + read-only live monitor window toggle.
                _runSweep = new ActionItem("VRSIM.RunSweep", "运行技术扫摆", "Program/rTeach", (System.Action)RunDiagnosticSweep,
                    MenuTools.ButtonTool, false, false)
                {
                    ContextFilter = Contexts.All, RibbonId = "VRSIM.RunSweep",
                    ToolSize = ToolSizingMode.ImageAndTextNormal,
                    ToolTipHeader = "运行技术扫摆", ToolTipContent = "仅诊断：严格 20s 扫摆+姿态重放验证（需当前无 VR 会话）"
                };
                Commands.RegisterActionItem(_runSweep, new UXSiteSetup { UXSiteIdPath = "VRSIMTab/VR", EntryId = _runSweep.Id, EntryCaption = "运行技术扫摆" });
                _monitorBtn = new ActionItem("VRSIM.Monitor", "实时监控", "Program/rTeach", (System.Action)ToggleMonitor,
                    MenuTools.ButtonTool, false, false)
                {
                    ContextFilter = Contexts.All, RibbonId = "VRSIM.Monitor",
                    ToolSize = ToolSizingMode.ImageAndTextNormal,
                    ToolTipHeader = "实时监控", ToolTipContent = "打开/关闭只读监控窗（SIM 提交图像预览 + 管线状态，不影响 VR 会话）"
                };
                Commands.RegisterActionItem(_monitorBtn, new UXSiteSetup { UXSiteIdPath = "VRSIMTab/VR", EntryId = _monitorBtn.Id, EntryCaption = "实时监控" });
                Log("RIBBON_REGISTERED VRSIMTab/VR EnterVR+ExitVR+RunSweep+Monitor");
            }
            catch (Exception ex)
            {
                Log("RIBBON_REGISTER_FAIL " + ex.GetType().Name + ": " + ex.Message);
            }
            WriteLoadedBinaries();
            try
            {
                string sim = VRSIM.EntryCalibration.SimulateAndReport();
                File.WriteAllText(Path.Combine(_logDir, "entry-calibration-simulation.json"), sim);
                Log("CALIBRATION_SIMULATION pass=" + (sim.Contains(Unquote()) ? "true" : "false"));
            }
            catch (Exception ex) { Log("CALIBRATION_SIM_FAIL " + ex.Message); }
            Msg("[VRSIM] v" + PluginVersion + " loaded — VRSIM 选项卡已就绪（Enter VR / Exit VR）");

            // XR platform skeleton (VRSIM-ARCH-DEVICE-01): device manager + Provider 1 runtime discovery
            try
            {
                _knownRegistry = new VRSIM.XR.KnownDeviceRegistry(_logDir);
                _deviceManager = new VRSIM.XR.XRDeviceManager(_knownRegistry);
                var rt = new VRSIM.XR.OpenXRRuntimeDiscovery();
                _deviceManager.AddProvider(rt);
                _deviceManager.StartAllDiscoveries();
                System.IO.File.WriteAllText(System.IO.Path.Combine(_logDir, "runtime-discovery.json"), rt.ToJson());
                Log("XR_PLATFORM_SKELETON runtimes=" + rt.GetDevices().Count);
            }
            catch (Exception ex) { Log("XR_PLATFORM_INIT_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        public void Exit()
        {
            _generation++;   // invalidate any in-flight Enter/ready/flow callbacks
            _activeFlowGen = -1;
            try { StopReadyTimer(); } catch { }
            try { StopPolling(); } catch { }
            // P1: StopProducerIfAny returning FALSE means the owner SDK transaction is NOT closed.
            // We may still deregister our OWN ribbon buttons + close the monitor + mark uninitialized,
            // but we must NEVER run native cleanup (Destroy) ahead of the owner stop.
            // Host3B2B: a retained OBS-SHARED source/coordinator NEVER takes the generic
            // producerClosed / direct UI VRSIM_Destroy branch below — it routes through the SAME
            // TeardownSession → A coordinator (no early release ahead of sourceFinalize, no
            // synchronous wait, no fake busy=false, no lost fault).
            var curShared = _producer;
            bool sharedRetained = (curShared != null && curShared.ObsSharedSelected) ||
                _obsSharedStopSource != null || _obsSharedNativeSource != null ||
                _obsSharedNativeTask != null;
            bool producerClosed = false;
            if (!sharedRetained)
            {
                try { producerClosed = StopProducerIfAny(); }
                catch { producerClosed = _producer == null; }
            }
            try { StopSweepIfAny(); } catch { }
            try { if (_enter != null && Commands != null) Commands.UnRegisterActionItem(_enter); } catch { }
            try { if (_exit != null && Commands != null) Commands.UnRegisterActionItem(_exit); } catch { }
            try { if (_runSweep != null && Commands != null) Commands.UnRegisterActionItem(_runSweep); } catch { }
            try { if (_monitorBtn != null && Commands != null) Commands.UnRegisterActionItem(_monitorBtn); } catch { }
            CloseMonitor();   // 0.1.3.5: plugin Exit closes the monitor (user ExitVR does NOT)
            _initialized = false;
            if (_creating)
            {
                // Create runs on the background Task: only cancel + queue here. The Task performs
                // Stop/Destroy itself once Create has finished — plugin Exit must NEVER Destroy
                // parallel with Create. Unchanged policy; no parallel Destroy added.
                _exitRequested = true;
                Log("PLUGIN_EXIT_QUEUED during create gen=" + _generation);
            }
            else if (sharedRetained)
            {
                _busy = true;
                Log("PLUGIN_EXIT_SHARED routed via TeardownSession/A coordinator — no UI VRSIM_Destroy; busy/source RETAINED");
                TeardownSession("plugin exit shared");
            }
            else if (!producerClosed)
            {
                // producer retained, owner transaction open → hold the pending native gate.
                // Keep _producer, keep _busy (blocks Enter), register deferred retry. No CAS start,
                // no VRSIM_Destroy, never claim cleaned.
                _busy = true;
                Log("PLUGIN_EXIT_DEFERRED producer_unclosed — native Destroy deferred");
                ScheduleProducerRetry();
            }
            else
            {
                // P1: entering the honest native cleanup path — retire the pending deferred
                // retry (timer + bound source fields) BEFORE the CAS gate, so a late tick can
                // never touch a newer flow once this Exit releases _busy.
                RetireProducerRetryTimer();
                if (Interlocked.CompareExchange(ref _teardownGate, 1, 0) == 0)
                {
                    try { int dc = VRSIM_Destroy(); Log("PLUGIN_EXIT destroy rc=" + dc); } catch { }
                    Interlocked.Exchange(ref _teardownGate, 0);
                }
                else Log("PLUGIN_EXIT teardown already running — Destroy skipped");
            }
        }

        // ---------------- Enter VR ----------------
        // 0.1.3.5: NORMAL entry goes straight to the live producer after readiness.
        // The strict camera-sweep/replay validation remains available ONLY as an explicit
        // diagnostic flow (Ribbon 运行技术扫摆) — its old thresholds are untouched and may
        // still FAIL; a failing diagnostic never blocks or fakes the normal path.
        private void EnterVR() { EnterVRCore(false); }

        private void EnterVRCore(bool diagnostic)
        {
            if (_busy) { Msg("[VRSIM] 正在处理中，请稍候"); return; }
            if (_vrActive) { Msg("[VRSIM] VR 已激活 — 请先 Exit VR"); return; }
            if (_producer != null)
            {
                // retained producer = pending failed cleanup; only Exit (StopProducerIfAny retry) clears it
                Msg("[VRSIM] 上次帧生产者清理失败被保留 — 请先点击 Exit VR 完成清理，再重试进入");
                return;
            }
            // NEXT-NORMAL-ENTRY-01: config-only shared preflight, pure managed (no SDK/GPU/PInvoke,
            // no pixel buffers). Runs BEFORE PreloadNative / SDK snapshot / nativeCreate, so an
            // invalid requested obs-shared tuple aborts with nothing created.
            bool requestedShared; string frozenSharedToken; string sharedReason;
            if (!TryValidateNormalSharedEntryConfig(out requestedShared, out frozenSharedToken, out sharedReason))
            {
                Msg("[VRSIM] obs-shared 正常入口配置无效 — VR 未启动（未预加载、未创建任何会话）: " + sharedReason);
                Log("NORMAL_SHARED_ENTRY_CONFIG_INVALID " + sharedReason);
                return;
            }
            if (requestedShared && diagnostic)
            {
                // obs-shared normal Enter bypasses the legacy self-test/sweep entirely;
                // never let the diagnostic flow mutate the main camera under shared config.
                Msg("[VRSIM] 已请求 obs-shared — 拒绝旧版诊断扫摆入口（请使用正常 Enter VR）");
                Log("DIAGNOSTIC_SWEEP_REFUSED_OBS_SHARED");
                return;
            }
            _busy = true;
            _diagnosticFlow = diagnostic;
            int gen = ++_generation;
            _normalSharedEntryFrozen = requestedShared;
            _normalSharedEntryToken = requestedShared ? frozenSharedToken : null;
            _normalSharedEntryFrozenGen = requestedShared ? gen : -1;
            // New flow generation → stale preview must never be shown as current:
            // clear the cache, bump the epoch (monitor resets on change), reset counters.
            lock (_previewLock)
            {
                _prevLeft = null; _prevRight = null;
                _prevW = _prevH = _prevStride = 0; _prevFs = _prevPs = 0; _prevUtc = DateTime.MinValue;
                _previewStoreCount = 0; _previewEpoch++;
            }
            Log("PREVIEW_CLEARED gen=" + gen);
            _exitRequested = false;
            _activeFlowGen = -1;
            _entrySnapshotOk = false;
            _msgOnce.Clear();   // per-session MsgOnce dedup (review item 4)
            Msg(diagnostic ? "[VRSIM] 运行技术扫摆 …" : "[VRSIM] Enter VR …");
            Log((diagnostic ? "DIAGNOSTIC_SWEEP" : "ENTER_VR") + " clicked gen=" + gen);
            if (!PreloadNative())
            {
                Msg("[VRSIM] 原生后端加载失败 — 见 vr-plugin.log");
                _busy = false;
                return;
            }
            _enterAt = DateTime.UtcNow;
            _visibleAnnounced = false;
            _quadAnnounced = false;

            // READYNESS FIX: snapshot the VC entry camera NOW (before SelfTest). The old
            // code only read it after a 60 s dispatcher-blocking wait; if no valid camera
            // exists, Enter fails explicitly.
            if (!CaptureEntryCameraSnapshot())
            {
                Msg("[VRSIM] 无法获取有效的 VC 入口相机，VR 未启动");
                _busy = false;
                return;
            }

            if (_normalSharedEntryFrozen)
            {
                // Shared normal Enter: hosted source guards were already validated by the pure-managed
                // preflight. This is NOT a legacy self-test and claims no content PASS — readiness and
                // entry calibration remain pending until the owned source is actually ready.
                Log("NORMAL_SHARED_PREFLIGHT_VALID_NOT_LEGACY_SELFTEST gen=" + _normalSharedEntryFrozenGen);
                Msg("[VRSIM] 正在连接头显并准备三维场景。请保持头显静止，完成入口视角校准。");
            }
            else
            {
                // PHASE 2/22 (RUNTIME-QA): the tracking self-test runs against the REAL VC camera
                // BEFORE any XR session is created. Any FAIL aborts VR startup — the user never
                // wears the headset to discover a broken axis again.
                var st = new TrackingSelfTest(this, _disp);
                if (!st.Run())
                {
                    File.WriteAllText(Path.Combine(_logDir, "selftest-summary.txt"), st.SummaryLog);
                    Msg("[VRSIM] Tracking Self-Test 未通过，VR 未启动（详见 selftest 日志）");
                    Log("SELFTEST_GATE_BLOCKED_VR");
                    _busy = false;
                    return;
                }
                File.WriteAllText(Path.Combine(_logDir, "selftest-summary.txt"), st.SummaryLog);
                Log("SELFTEST_GATE_PASS -> starting XR" + (diagnostic ? " (diagnostic flow)" : ""));
                Msg("[VRSIM] Tracking Self-Test 全部通过 — 正在启动 VR…");
            }
            if (diagnostic)
                Msg("[VRSIM] 诊断流：请戴上头显，就绪后画面将自动左右摇摆 20 秒（严格机器验证，可能 FAIL，不影响正常入口）");
            else if (!_normalSharedEntryFrozen)
                Msg("[VRSIM] 请戴上头显：进入后就绪即开始实时画面，静置约 1.5 秒锁定入口视角（正常入口不自动扫摆）");

            // Session create runs in the background; readiness is then awaited on the UI
            // thread via DispatcherTimer (READYNESS FIX — no Thread.Sleep on the dispatcher).
            // Only create rc==0 AND EnableSceneMode rc==0 AND state 5/6 start the sweep.
            // Set BEFORE Task.Run is dispatched: an Exit clicked while the task is still unscheduled
            // must queue (never see "VR 未激活" and do nothing).
            _creating = true;
            Task.Run(() =>
            {
                int rc = -99, sr = -99;
                try
                {
                    VRSIM_SetLoaderPathW(null);
                    rc = VRSIM_Create();
                    Log("VRSIM_Create rc=" + rc + " lastError=" + NativeError());
                    if (rc == 0)
                    {
                        VRSIM_Start();
                        // PRODUCT-02: switch the swapchains to the VC scene content size
                        sr = VRSIM_EnableSceneMode(VCFrameProducer.FrameW, VCFrameProducer.FrameH);
                        Log("EnableSceneMode(" + VCFrameProducer.FrameW + "x" + VCFrameProducer.FrameH + ") rc=" + sr);
                        if (sr == 0) BeginReadyWait(gen, diagnostic);
                    }
                }
                catch (Exception ex)
                {
                    Log("ENTER_FAIL " + ex.GetType().Name + ": " + ex.Message);
                    Msg("[VRSIM] VR 启动异常: " + ex.Message);
                    if (rc == -99) rc = -1;
                    if (rc == 0 && sr == -99) sr = -1;   // exception after create → session may exist
                }
                _creating = false;

                // Race (Exit during create): cleanup is QUEUED, never run parallel with Create.
                bool canceled = _exitRequested || gen != _generation;
                _exitRequested = false;
                if (canceled)
                {
                    Log("ENTER_CANCELED_AFTER_CREATE gen=" + gen + " curGen=" + _generation + " rc=" + rc + " enableRc=" + sr);
                    if (rc == 0) TeardownSession("canceled during create (gen " + gen + ")");
                    else _busy = false;
                    return;
                }
                if (rc == 0 && sr == 0)
                {
                    if (_readyWaiting) return;   // ready timer owns the flow from here
                    Log("READY_WAIT_NOT_STARTED");
                    TeardownSession("ready timer unavailable");
                    return;
                }
                if (rc != 0)
                {
                    if (rc == 1) Msg("[VRSIM] HMD 未连接 — 请确认 XR 运行时/串流已激活后再点 Enter VR");
                    else Msg("[VRSIM] VR 启动失败: " + NativeError());
                    _busy = false;   // no session was created — nothing to clean up
                    return;
                }
                // session created but EnableSceneMode failed → must NOT enter readiness
                Msg("[VRSIM] EnableSceneMode 失败 (rc=" + sr + ") — 正在清理 XR 会话");
                TeardownSession("EnableSceneMode rc=" + sr);
            });
        }

        // NEXT-NORMAL-ENTRY-01: configuration-only shared-entry validator. PRIVATE STATIC so cold
        // reflection QA can invoke it without constructing any host or touching Application. Pure
        // managed: reads environment strings and frozen statics only — NO PInvoke, NO SDK objects,
        // NO capture, NO pixel buffers, NO GPU. Small immutable string tokens only.
        // Return semantics: true = "not selected" (legacy flow unchanged) OR valid obs-shared tuple;
        // false = obs-shared requested but invalid (never claim an env read exception is valid).
        // No Trim, no fallback, no environment writes; every token compares exactly (Ordinal).
        private static bool TryValidateNormalSharedEntryConfig(
            out bool requestedShared, out string frozenToken, out string reason)
        {
            requestedShared = false;
            frozenToken = null;
            reason = null;

            string readback;
            try
            {
                readback = Environment.GetEnvironmentVariable("VRSIM_READBACK_PIPELINE");
            }
            catch (Exception ex)
            {
                // First input unreadable: cannot know the request, so fail as invalid shared
                // rather than silently slipping into the legacy flow.
                requestedShared = true;
                reason = "VRSIM_READBACK_PIPELINE read failed " + ex.GetType().Name + ": " + ex.Message;
                return false;
            }
            if (!string.Equals(readback, "obs-shared", StringComparison.Ordinal))
            {
                // Not selected: read nothing else here; the old nonshared flow stays unchanged.
                reason = "not-selected (VRSIM_READBACK_PIPELINE is not exactly 'obs-shared')";
                return true;
            }

            requestedShared = true;
            string backend, projection, scheduler, profileEnv, tickMs, rawColorspace;
            try
            {
                backend = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_BACKEND");
                projection = Environment.GetEnvironmentVariable("VRSIM_PROJECTION_PIPELINE");
                scheduler = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_SCHEDULER");
                profileEnv = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_PROFILE");
                tickMs = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_TICK_MS");
                rawColorspace = Environment.GetEnvironmentVariable("VRSIM_RAW_COLORSPACE");
            }
            catch (Exception ex)
            {
                reason = "shared configuration env read failed " + ex.GetType().Name + ": " + ex.Message;
                return false;
            }

            if (!string.Equals(backend, "hosted-d3d9", StringComparison.Ordinal))
            {
                reason = "VRSIM_CAPTURE_BACKEND is not exactly 'hosted-d3d9'";
                return false;
            }
            if (!string.Equals(projection, "gpu", StringComparison.Ordinal))
            {
                reason = "VRSIM_PROJECTION_PIPELINE is not exactly 'gpu'";
                return false;
            }
            if (!string.Equals(scheduler, "kernel-render-120", StringComparison.Ordinal))
            {
                reason = "VRSIM_CAPTURE_SCHEDULER is not exactly 'kernel-render-120'";
                return false;
            }
            if (!string.Equals(tickMs, "1", StringComparison.Ordinal))
            {
                reason = "VRSIM_CAPTURE_TICK_MS is not exactly '1'";
                return false;
            }

            // Existing compile-selected binding edge: 1536 without the macro, 2048 with it.
            // NO new macro/ABI and no new dimensions are introduced here.
            string expectedProfile = "native-" + SharedGpuTicketEdgeV1 + "x" + SharedGpuTicketEdgeV1;
            if (!string.Equals(profileEnv, CaptureProfile.Requested, StringComparison.Ordinal))
            {
                reason = "VRSIM_CAPTURE_PROFILE env value differs from the frozen CaptureProfile.Requested " +
                    "(config rewritten after static init, or unreadable at init)";
                return false;
            }
            if (!string.Equals(CaptureProfile.Selected, expectedProfile, StringComparison.Ordinal))
            {
                reason = "CaptureProfile.Selected is not exactly '" + expectedProfile + "' (got '" +
                    CaptureProfile.Selected + "')";
                return false;
            }
            if ((uint)VCFrameProducer.FrameW != SharedGpuTicketEdgeV1 ||
                (uint)VCFrameProducer.FrameH != SharedGpuTicketEdgeV1)
            {
                reason = "VCFrameProducer.FrameW/FrameH do not equal binding edge " + SharedGpuTicketEdgeV1;
                return false;
            }

            if (!string.Equals(rawColorspace, "legacy", StringComparison.Ordinal) &&
                !string.Equals(rawColorspace, "srgb", StringComparison.Ordinal) &&
                rawColorspace != null)
            {
                reason = "VRSIM_RAW_COLORSPACE must be absent, exactly 'legacy' or exactly 'srgb'";
                return false;
            }

            // Managed wire-layout check only (Marshal size/offset validation, cached) — no PInvoke.
            if (!SharedGpuWireLayoutValid())
            {
                reason = "managed SharedGpuWireLayoutValid() reported an invalid wire layout";
                return false;
            }

            // Freeze every exact scalar (color included but NOT altered) into one immutable token.
            frozenToken = string.Join("|", new string[]
            {
                "obs-shared",
                backend,
                projection,
                scheduler,
                tickMs,
                CaptureProfile.Requested ?? string.Empty,
                rawColorspace ?? "(absent)",
                SharedGpuTicketEdgeV1.ToString(System.Globalization.CultureInfo.InvariantCulture)
            });
            reason = "valid obs-shared normal entry configuration (preflight only — not a legacy self-test PASS)";
            return true;
        }

        private bool PreloadNative()
        {
            if (_nativeDir == null) return false;
            string p = Path.Combine(_nativeDir, NativeDllName);
            if (!File.Exists(p))
            {
                Log("NATIVE_MISSING " + p);
                return false;
            }
            IntPtr h = LoadLibraryW(p);
            Log("LoadLibrary " + p + " -> 0x" + h.ToInt64().ToString("X") + (h == IntPtr.Zero ? " gle=" + Marshal.GetLastWin32Error() : ""));
            return h != IntPtr.Zero;
        }

        // ---------------- readiness (READYNESS FIX) ----------------
        private bool CaptureEntryCameraSnapshot()
        {
            try
            {
                var vp = VisualComponents.UX.Input.HelperViewport.ActiveViewport;
                if (vp == null || vp.Window == null) { Log("ENTRY_SNAPSHOT_FAIL no_active_viewport"); return false; }
                var cam = vp.Window.Camera;
                if (cam == null) { Log("ENTRY_SNAPSHOT_FAIL no_camera"); return false; }
                var m = cam.ViewMatrix;
                double fov = cam.FieldOfViewY;
                double gaze = double.NaN;
                try { gaze = cam.GazeLength; } catch { }
                if (!FiniteD(m.Px) || !FiniteD(m.Py) || !FiniteD(m.Pz) || !FiniteD(m.Nx) || !FiniteD(m.Oy) || !FiniteD(m.Az)
                    || !FiniteD(fov) || fov <= 0)
                { Log("ENTRY_SNAPSHOT_FAIL invalid_camera"); return false; }
                _entryViewForSweep = m;
                _entryFovForSweep = fov;
                _entryGazeLength = gaze;
                _entrySnapshotOk = true;
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                Log("ENTRY_SNAPSHOT captured pos=(" + m.Px.ToString("0.0", ci) + "," + m.Py.ToString("0.0", ci) + "," + m.Pz.ToString("0.0", ci) +
                    ") fovY=" + fov.ToString("0.0", ci) + " gazeLength=" + (FiniteD(gaze) ? gaze.ToString("0.0", ci) : "n/a"));
                return true;
            }
            catch (Exception ex) { Log("ENTRY_SNAPSHOT_FAIL " + ex.GetType().Name + ": " + ex.Message); return false; }
        }

        private static bool FiniteD(double d) { return !double.IsNaN(d) && !double.IsInfinity(d); }

        private void BeginReadyWait(int gen, bool diagnostic)
        {
            _readyStartedUtc = DateTime.UtcNow;
            try
            {
                _disp.Invoke((System.Action)(() =>
                {
                    if (gen != _generation) return;   // canceled while create ran — Task tail owns teardown
                    _readyTimer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
                    _readyTimer.Tick += (s, e) => ReadyTick(gen, diagnostic);
                    _readyWaiting = true;
                    _readyTimer.Start();
                }));
            }
            catch (Exception ex) { Log("READY_WAIT_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        private void ReadyTick(int gen, bool diagnostic)
        {
            try
            {
                if (gen != _generation) { StopReadyTimer(); return; }
                int state = VRSIM_GetState();
                if (state == 5 || state == 6) { ReadyReached(gen, state, diagnostic); return; }
                if (state == 7 || state == 8) { ReadyFailed(gen, "session state " + StateName(state)); return; }
                if ((DateTime.UtcNow - _readyStartedUtc).TotalSeconds >= 30.0)
                { ReadyFailed(gen, "timeout 30s, last state " + StateName(state)); return; }
            }
            catch (Exception ex)
            {
                ReadyFailed(gen, "ready tick exception " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        private void ReadyReached(int gen, int state, bool diagnostic)
        {
            // 09-reviewed03: a late Ready for an exited/re-entered flow must NOT rewrite
            // _activeFlowGen or stop the NEW session's ready timer — guard BEFORE any state touch.
            if (gen != _generation || _teardownGate != 0)
            {
                Log("READY_REACHED_IGNORED_STALE gen=" + gen + " currentGen=" + _generation);
                return;
            }
            int originalFlowGeneration = gen;
            _activeFlowGen = originalFlowGeneration;
            StopReadyTimer();
            if (!_entrySnapshotOk) { ReadyFailed(gen, "entry camera snapshot missing"); return; }
            double secs = (DateTime.UtcNow - _enterAt).TotalSeconds;
            Log((diagnostic ? "DIAG_READY" : "NORMAL_READY") + " state=" + StateName(state) +
                " afterSeconds=" + secs.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
            if (diagnostic)
            {
                // diagnostic flow only: the strict sweep/replay validation chain (thresholds unchanged)
                _sweep = new CameraSweepController(this, _disp);
                _sweep.Run(_entryViewForSweep, _entryFovForSweep, _entryGazeLength);
                BeginPolling();
                _vrActive = true;
                _busy = false;
                Msg("[VRSIM] VR 后端已启动（会话 " + StateName(state) + "，" + secs.ToString("0.0") + "s）— 开始相机扫摆动态链");
                return;
            }
            // 0.1.3.5 NORMAL ENTRY: producer directly on the Enter snapshot; the head stays
            // STATIC until EntryCalibration locks (tracking-valid + focused + 1.5 s stability).
            bool producerStarted = false;
            bool staleFlow = false;
            string startFailure = null;
            VCFrameProducer source = null;
            int hostedStartGen = -1;
            try
            {
                _disp.Invoke((System.Action)(() =>
                {
                    if (originalFlowGeneration != _generation || _teardownGate != 0) { staleFlow = true; return; }
                    if (_producer != null)
                    {
                        Log("NORMAL_ENTRY_PRODUCER_REFUSED existing producer retained (pending cleanup) — not overwriting");
                        return;
                    }
                    // NEXT-NORMAL-ENTRY-01: frozen shared generation revalidates its exact token
                    // BEFORE any VCFrameProducer is constructed. A config change here aborts the
                    // original flow via ReadyFailed; no producer exists yet, so nothing must be cleaned.
                    if (_normalSharedEntryFrozen && originalFlowGeneration == _normalSharedEntryFrozenGen)
                    {
                        bool stillShared; string nowToken; string whyNot;
                        if (!TryValidateNormalSharedEntryConfig(out stillShared, out nowToken, out whyNot) ||
                            !stillShared || !string.Equals(nowToken, _normalSharedEntryToken, StringComparison.Ordinal))
                        {
                            startFailure = "obs-shared entry config changed before ready — " +
                                (whyNot ?? "not-selected now") + " (no producer constructed)";
                            Log("NORMAL_SHARED_READY_REVALIDATE_FAILED gen=" + originalFlowGeneration +
                                " reason=" + (whyNot ?? "not-selected now"));
                            return;
                        }
                    }
                    source = new VCFrameProducer(this, _disp);
                    _producer = source;
                    source.CaptureEntryState(_entryViewForSweep, _entryFovForSweep, _entryGazeLength);
                    // Hosted Start can pump Exit/Stop and THROWS when canceled — never a silent return.
                    source.Start();
                    if (originalFlowGeneration != _generation || _activeFlowGen != originalFlowGeneration ||
                        _teardownGate != 0 || !ReferenceEquals(_producer, source))
                    { staleFlow = true; return; }
                    if (source.HostedSelected)
                    {
                        if (!source.CaptureAdmissionOpen || !string.IsNullOrEmpty(source.CaptureBackendFailureReason))
                        {
                            startFailure = "hosted capture backend not admitted at Start completion (" +
                                (string.IsNullOrEmpty(source.CaptureBackendFailureReason)
                                    ? "admission closed" : "reason=" + source.CaptureBackendFailureReason) + ")";
                            return;
                        }
                        // Admission open + no recorded failure + backend NOT ready yet is the legal
                        // pending state — a valid Start, not a failure.
                        hostedStartGen = source.HostedStartGeneration;
                    }
                    producerStarted = true;
                }));
            }
            catch (Exception ex)
            {
                if (originalFlowGeneration != _generation || _teardownGate != 0 || !ReferenceEquals(_producer, source))
                {
                    // Original flow canceled/torn down mid-Start: end the OLD ReadyReached silently —
                    // no producerStarted claim, no BeginPolling, no _vrActive/_busy restore, no
                    // ready message, and never ReadyFailed against the NEW flow.
                    Log("READY_REACHED_ABANDONED_STALE gen=" + originalFlowGeneration +
                        " Start exception on superseded flow " + ex.GetType().Name + ": " + ex.Message);
                    return;
                }
                startFailure = "producer Start exception " + ex.GetType().Name + ": " + ex.Message;
            }
            if (staleFlow)
            {
                Log("READY_REACHED_ABANDONED_STALE gen=" + originalFlowGeneration + " flow superseded during producer Start");
                return;
            }
            if (!producerStarted)
            {
                // never swallow the refusal/failure into a successful Ready — ReadyFailed tears XR down
                if (startFailure != null) { ReadyFailed(originalFlowGeneration, startFailure); return; }
                ReadyFailed(originalFlowGeneration, "existing VC producer retained (清理失败待重试) — 请先点 Exit VR 完成清理");
                return;
            }
            Log("NORMAL_ENTRY_PRODUCER_STARTED snapshotGen=" + originalFlowGeneration + " state=" + StateName(state));
            // FINAL pre-poll recheck (09-reviewed03): pure managed conditions only, no SDK call interleaved.
            if (originalFlowGeneration != _generation || _activeFlowGen != originalFlowGeneration ||
                _teardownGate != 0 || !ReferenceEquals(_producer, source))
            {
                Log("READY_REACHED_ABANDONED_STALE gen=" + originalFlowGeneration + " flow superseded before polling began");
                return;
            }
            bool hostedPending = false;
            if (source.HostedSelected)
            {
                if (!source.CaptureAdmissionOpen || !string.IsNullOrEmpty(source.CaptureBackendFailureReason) ||
                    source.HostedStartGeneration != hostedStartGen)
                {
                    ReadyFailed(originalFlowGeneration, "hosted capture backend failed before ready (" +
                        (string.IsNullOrEmpty(source.CaptureBackendFailureReason)
                            ? "admission closed" : "reason=" + source.CaptureBackendFailureReason) + ")");
                    return;
                }
                hostedPending = !source.IsCaptureBackendReady;   // legal pending — never claimed as live content
            }
            BeginPolling();
            _vrActive = true;
            _busy = false;
            if (_normalSharedEntryFrozen && originalFlowGeneration == _normalSharedEntryFrozenGen)
            {
                // Shared ready messages: no legacy-sweep instruction, no self-test PASS,
                // no usable-FPS or live-pixel claim before the owned source is ready.
                if (hostedPending)
                    Msg("[VRSIM] 头显已连接，正在准备三维场景；画面就绪后开始视角校准。");
                else
                    Msg("[VRSIM] 场景后端已就绪，正在等待画面并完成视角校准。");
            }
            else if (hostedPending)
                Msg("[VRSIM] XR 会话已启动（" + StateName(state) + "，" + secs.ToString("0.0") + "s）— owned capture 等待实际 Ready（await_hosted_ready），实时内容尚未确认运行；如需技术验证请点『运行技术扫摆』");
            else
                Msg("[VRSIM] VR 已启动（" + StateName(state) + "，" + secs.ToString("0.0") + "s）— 实时管线运行中；如需技术验证请点『运行技术扫摆』");
        }

        private void ReadyFailed(int gen, string reason)
        {
            if (gen != _generation) { Log("READY_FAIL_IGNORED_STALE gen=" + gen + " reason=" + reason); return; }
            StopReadyTimer();
            Log("READY_FAIL " + reason);
            Msg("[VRSIM] VR 未就绪（" + reason + "）— 未启动实时管线，正在清理本插件创建的 XR 会话，清理完成后可重试 Enter VR");
            TeardownSession("ready_failed: " + reason);
        }

        /// <summary>Receiver for the hosted-d3d9 capture failure notification (the producer-side
        /// post-busy async call is wired by a later card). Pure managed validation only; a
        /// legitimate notification goes through the EXISTING ReadyFailed/current-flow teardown and
        /// nothing else. failureGeneration is the producer's ORIGINAL hosted Start (capture)
        /// generation — it is never compared numerically against the host flow generation.</summary>
        internal void ReportHostedCaptureFailure(VCFrameProducer source, int failureGeneration, string reason)
        {
            if (source == null || reason == null) return;
            if (_activeFlowGen < 0)
            {
                // Manual controlled QA: the Root harness exclusively owns Stop/Destroy — record only.
                Log("HOSTED_CAPTURE_FAILURE_QA_RECORD captureGen=" + failureGeneration + " reason=" + reason);
                return;
            }
            if (!ReferenceEquals(source, _producer))
            { Log("HOSTED_CAPTURE_FAILURE_IGNORED old producer source identity captureGen=" + failureGeneration); return; }
            if (!source.HostedCaptureFailureNotificationPending ||
                source.HostedFailureGenerationForMonitor != failureGeneration ||
                !string.Equals(reason, source.HostedCaptureFailureReasonForMonitor, StringComparison.Ordinal))
            { Log("HOSTED_CAPTURE_FAILURE_IGNORED not-pending/stale-capture-gen/reason-mismatch captureGen=" + failureGeneration + " reason=" + reason); return; }
            if (source.CaptureAdmissionOpen)
            { Log("HOSTED_CAPTURE_FAILURE_NOT_FATAL admission still open captureGen=" + failureGeneration); return; }
            if (_activeFlowGen != _generation || !_vrActive || _teardownGate != 0)
            { Log("HOSTED_CAPTURE_FAILURE_IGNORED flow-not-current/stopped/teardown captureGen=" + failureGeneration); return; }
            Log("HOSTED_CAPTURE_FAILURE_FATAL captureGen=" + failureGeneration +
                " flowGen=" + _activeFlowGen + " reason=" + reason);
            ReadyFailed(_activeFlowGen, "hosted capture backend failure: " + reason);
        }

        private void StopReadyTimer()
        {
            _readyWaiting = false;
            var t = _readyTimer;
            if (t == null) return;
            _readyTimer = null;
            try { if (_disp.CheckAccess()) t.Stop(); else _disp.Invoke((System.Action)(() => t.Stop())); }
            catch { }
        }

        // Stop the running sweep/replay (timer killed + camera restored) — a live DispatcherTimer
        // must never keep driving the VC camera after Exit/teardown. Safe from any thread.
        internal void StopSweepIfAny()
        {
            var s = _sweep;
            if (s == null) return;
            _sweep = null;
            try { _disp.Invoke((System.Action)(() => s.CancelAndRestore())); }
            catch (Exception ex) { Log("SWEEP_CANCEL_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        /// <summary>Returns TRUE when no producer is retained afterwards (none, or stopped+restored
        /// successfully). On StopAndRestore failure the producer stays in _producer so the next
        /// Exit can retry — the failure is logged AND surfaced, never claimed as cleaned.</summary>
        internal bool StopProducerIfAny()
        {
            var p = _producer;
            if (p == null) return true;
            if (p.ObsSharedSelected)
            {
                // OBS-SHARED coordinator branch (Host3B2A). SourceReady is NOT whole stop: _producer
                // stays retained until p.ObsSharedWholeStopVerified; a generic successful return must
                // NEVER clear shared p. The documented pending is treated as pending WITHOUT a repeated
                // Msg popup; ObsSharedStopFailureEvidence / LastObsSharedCaptureFault and the actual
                // exceptions inside the producer are preserved. _busy stays true while pending.
                if (!ReferenceEquals(_obsSharedStopSource, p))
                {
                    // Replacement refusal: while an OLD shared coordinator/native source/task is still
                    // retained, a different p may NEVER overwrite or reset the identity fields.
                    if (_obsSharedStopSource != null || _obsSharedNativeSource != null ||
                        _obsSharedNativeTask != null)
                    {
                        _busy = true;
                        Log("OBS_SHARED_STOP_REPLACE_REFUSED — old shared coordinator/native source/task still RETAINED; new source NOT latched");
                        return false;
                    }
                    // Truly fresh shared source with NOTHING pending: reset THIS generation's
                    // coordinator state exactly ONCE, BEFORE the latch. Duplicate callers/retries take
                    // the latched path below and never reset.
                    _obsSharedTeardownCompleted = false;
                    _obsSharedLastNativeDestroyRc = int.MinValue;
                    _obsSharedNativeDestroyState = int.MinValue;
                    _obsSharedNativeTaskReturned = false;
                    _obsSharedConfirmRetries = 0;
                    _obsSharedConfirmBeginQpc = 0L;
                    _obsSharedHostTerminal = false;
                    _obsSharedHostError = null;
                    // FIRST stop attempt: latch the shared coordinator identity BEFORE StopAndRestore.
                    _obsSharedStopSource = p;
                    try
                    {
                        _disp.Invoke((System.Action)(() => p.StopAndRestore()));
                        if (p.ObsSharedWholeStopVerified && ReferenceEquals(_producer, p))
                        {
                            _producer = null;
                            _obsSharedStopSource = null;
                            Log("VC_PRODUCER obs_shared whole stop verified — exact shared producer cleared");
                            return true;
                        }
                        _busy = true;
                        Log("OBS_SHARED_STOP_PENDING first attempt returned unverified sourceReady=" +
                            (p.ObsSharedSourceReadyForNativeTeardown ? "true" : "false") +
                            " wholeVerified=false — producer RETAINED, no generic clear");
                        return false;
                    }
                    catch (Exception ex)
                    {
                        _busy = true;
                        if (p.ObsSharedStopTerminal)
                        {
                            RetireProducerRetryTimer();   // terminal stops only its own timer
                            Log("OBS_SHARED_STOP_TERMINAL first attempt " + ex.GetType().Name + ": " + ex.Message +
                                " — own retry stopped; source/busy/evidence RETAINED");
                        }
                        else
                            Log("OBS_SHARED_STOP_PENDING first attempt " + ex.GetType().Name + ": " + ex.Message +
                                " — producer RETAINED (documented pending; no repeated Msg popup)");
                        return false;
                    }
                }
                // Duplicate first-stop callers coalesce on the same retained source; only a retry-tick
                // context drives the source-step continuation (the first attempt's internal step already
                // ran exactly once — never a second TryStep from this same StopProducerIfAny turn).
                if (Volatile.Read(ref _producerRetryReentrancy) == 0) return false;
                return ObsSharedSourceStopContinuation(p);
            }
            try
            {
                _disp.Invoke((System.Action)(() => p.StopAndRestore()));
                // StopAndRestore may pump the dispatcher: _producer can be replaced (or already
                // cleared) by a nested path while this call runs. Clear ONLY our own p; a
                // replacement producer must never be nulled here — report not-closed honestly.
                if (ReferenceEquals(_producer, p)) _producer = null;   // cleared ONLY after a successful stop+restore of OUR p
                else if (_producer != null)
                {
                    Log("PRODUCER_STOP_REPLACED — a newer producer took over _producer; left untouched, NOT claiming closed");
                    return false;
                }
                Log("VC_PRODUCER stopped+restored");
                return true;
            }
            catch (Exception ex)
            {
                Log("PRODUCER_STOP_FAIL " + ex.GetType().Name + ": " + ex.Message + " — producer RETAINED for Exit retry");
                Msg("[VRSIM] 帧生产者清理失败，已保留 — 请点击 Exit VR 重试清理后再进入");
                return false;
            }
        }

        // P1 deferred continuation (shared by Exit and TeardownSession). Arms ONE retained
        // DispatcherTimer that ONLY retries the owner StopAndRestore. It never starts native
        // cleanup itself; on success it hands off to the existing unified Stop->Destroy gate.
        // Multiple Exit/teardown requests coalesce onto the one pending retry.
        private void ScheduleProducerRetry()
        {
            var originalSource = _producer;
            if (originalSource == null) return;   // nothing retained → nothing pending
            if (_disp.HasShutdownStarted || _disp.HasShutdownFinished)
            {
                // Dispatcher is closing: keep the producer + log the unclosed state honestly.
                // NEVER fake a successful cleanup here.
                Log("PRODUCER_RETRY_SKIPPED dispatcher_shutdown producer RETAINED unclosed");
                return;
            }
            try
            {
                _disp.Invoke((System.Action)(() =>
                {
                    // Requests for the SAME retained producer coalesce onto the one armed timer.
                    // A pending timer bound to a DIFFERENT (older) producer is retired here and
                    // re-armed for the current source, so the current owner stop is always
                    // retried while older ticks self-drop on source+timer identity.
                    if (_producerRetryTimer != null && ReferenceEquals(_producerRetrySource, originalSource))
                        return;   // already armed for this exact producer → coalesce
                    if (_producerRetryTimer != null) RetireProducerRetryTimer();
                    var thisTimer = new System.Windows.Threading.DispatcherTimer(
                        System.Windows.Threading.DispatcherPriority.Background)
                    { Interval = TimeSpan.FromMilliseconds(100) };
                    thisTimer.Tick += (s, e) => ProducerRetryTick(originalSource, thisTimer);
                    _producerRetryTimer = thisTimer;
                    _producerRetrySource = originalSource;
                    thisTimer.Start();
                    Log("PRODUCER_RETRY_ARMED native cleanup deferred pending owner stop");
                }));
            }
            catch (Exception ex) { Log("PRODUCER_RETRY_ARM_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        private void ProducerRetryTick(VCFrameProducer originalSource,
            System.Windows.Threading.DispatcherTimer thisTimer)
        {
            // The owner StopAndRestore call may pump the dispatcher; guard against tick re-entry.
            if (Interlocked.CompareExchange(ref _producerRetryReentrancy, 1, 0) != 0) return;
            try
            {
                // P1: this tick is BOUND to the producer that armed it and to its own timer
                // instance. If either was replaced (a newer cleanup retired/re-armed the retry,
                // or the producer was closed elsewhere) this callback is STALE: stop ONLY its
                // own timer, never touch the newer producer/_busy/flow, never run native cleanup.
                if (!ReferenceEquals(_producer, originalSource) || !ReferenceEquals(_producerRetryTimer, thisTimer))
                {
                    StopProducerRetryTimer(thisTimer);
                    Log("PRODUCER_RETRY_STALE source or timer replaced — deferred handoff dropped");
                    return;
                }
                if (_creating) return;                                // never interfere with in-flight create
                if (Volatile.Read(ref _teardownGate) != 0) return;    // another cleanup running → keep waiting
                if (originalSource.ObsSharedSelected)
                {
                    // Host3B2B: a shared source NEVER enters the generic StopProducerIfAny / null
                    // check / TeardownSession handoff here. The A continuation drives at most ONE
                    // source step / direct native handoff / bounded (32 from the actual task return)
                    // Confirm retry for THIS timer's own source, re-checking identity after its
                    // internal pumping calls, and retires only this timer on terminal/deadline while
                    // retaining source/busy. Timer absence never proves closure; a recorded native
                    // task pending its real completion is never rearmed or repeated.
                    ObsSharedSourceStopContinuation(originalSource);
                    return;
                }
                if (!StopProducerIfAny()) { Log("PRODUCER_RETRY_STILL_PENDING owner stop open"); return; }
                // Stop may have pumped: re-check timer identity and that the ORIGINAL source is the
                // one that got closed (StopProducerIfAny returns true only with no replacement).
                // A retired/replaced state means an honest cleanup path owns teardown now.
                if (!ReferenceEquals(_producerRetryTimer, thisTimer) || _producer != null)
                {
                    Log("PRODUCER_RETRY_HANDOFF_DROPPED timer retired or source not closed by this tick");
                    return;
                }
                StopProducerRetryTimer(thisTimer);
                TeardownSession("deferred producer closed");   // exactly once via the existing gate
            }
            finally { Interlocked.Exchange(ref _producerRetryReentrancy, 0); }
        }

        // Stop a SPECIFIC retry timer; clear the field refs only while they still point to it —
        // a stale tick must never clear a timer (or source) a newer retry owns.
        private void StopProducerRetryTimer(System.Windows.Threading.DispatcherTimer t)
        {
            if (t == null) return;
            try { if (_disp.CheckAccess()) t.Stop(); else _disp.Invoke((System.Action)(() => t.Stop())); }
            catch { }
            if (ReferenceEquals(_producerRetryTimer, t))
            {
                _producerRetryTimer = null;
                _producerRetrySource = null;
            }
        }

        // Claim-and-retire used by the honest native cleanup paths BEFORE their CAS gate:
        // clear the field refs FIRST so any pending (possibly queued) tick sees itself as stale,
        // then Stop. Never touches _producer/_busy — the caller's cleanup path owns those.
        private void RetireProducerRetryTimer()
        {
            var t = _producerRetryTimer;
            if (t == null) return;
            _producerRetryTimer = null;
            _producerRetrySource = null;
            try { if (_disp.CheckAccess()) t.Stop(); else _disp.Invoke((System.Action)(() => t.Stop())); }
            catch { }
        }

        // ---------------- Host3B2A OBS-SHARED teardown coordinator (prepared; B wires entry paths) ----------------

        // UI-thread source-step continuation for the retained shared coordinator. Requires
        // _disp.CheckAccess and the exact coordinator source == current _producer; a pending
        // _creating only queues the existing _exitRequested. At most ONE
        // TryStepObsSharedSourceStop per call (the Producer's own 32-step/5000 ms deadline owns
        // the source budget); on source-ready p (STILL retained) hands directly to the native
        // helper. A recorded native task is NEVER started again — only Confirm retries with the
        // SAME actual dc/state run, bounded 32 tries / 5000 ms from the actual task return.
        // Terminal states stop ONLY this timer and retain source/busy/error.
        private bool ObsSharedSourceStopContinuation(VCFrameProducer p)
        {
            if (!_disp.CheckAccess()) return false;
            if (!ReferenceEquals(_producer, p) || !ReferenceEquals(_obsSharedStopSource, p))
                return false;   // exact current producer AND this-generation latch — no generic producer-null dependency
            if (_creating) { _exitRequested = true; return false; }
            try
            {
                if (_obsSharedHostTerminal)
                {
                    Log("OBS_SHARED_SOURCE_STEP_HOST_TERMINAL sticky — no re-step/re-confirm; refs/busy RETAINED");
                    RetireProducerRetryTimer();   // never re-arm after a host terminal
                    return false;
                }
                if (_obsSharedTeardownCompleted) return false;  // completed by the result dispatch already
                if (p.ObsSharedStopTerminal)
                {
                    RetireProducerRetryTimer();   // terminal stops only its own timer
                    Log("OBS_SHARED_SOURCE_STEP_TERMINAL own retry timer stopped; source/busy/evidence RETAINED");
                    return false;
                }
                if (_obsSharedNativeTask != null)
                {
                    // an actual native task was recorded — native cleanup is never re-entered here,
                    // and an OLD native result may NEVER drive a NEWER producer
                    if (!ReferenceEquals(_obsSharedNativeSource, p)) return false;
                    if (!_obsSharedNativeTaskReturned || _obsSharedLastNativeDestroyRc != 0 ||
                        _obsSharedNativeDestroyState != 0)
                        return false;   // still running, or the recorded dc/state forbids confirmation
                    _obsSharedConfirmRetries = checked(_obsSharedConfirmRetries + 1);
                    if (_obsSharedConfirmRetries > 32 ||
                        (_obsSharedConfirmBeginQpc != 0L &&
                         Stopwatch.GetTimestamp() - _obsSharedConfirmBeginQpc > Stopwatch.Frequency * 5))
                    {
                        MarkObsSharedHostTerminal("confirm_budget_expired retries=" +
                            _obsSharedConfirmRetries.ToString(System.Globalization.CultureInfo.InvariantCulture) +
                            " — task/result/source identity RETAINED; native and confirmation never re-run", null);
                        return false;
                    }
                    bool confirmedR;
                    try { confirmedR = p.ConfirmObsSharedNativeDestroyed(_obsSharedLastNativeDestroyRc, _obsSharedNativeDestroyState); }
                    catch (Exception cex)
                    {
                        MarkObsSharedHostTerminal("confirm_retry_exception", cex);
                        return false;
                    }
                    if (confirmedR && p.ObsSharedWholeStopVerified)
                    {
                        _obsSharedTeardownCompleted = true;
                        if (ReferenceEquals(_producer, p)) _producer = null;
                        _obsSharedStopSource = null;
                        _obsSharedNativeSource = null;
                        _obsSharedNativeTask = null;
                        Interlocked.Exchange(ref _teardownGate, 0);
                        RetireProducerRetryTimer();
                        _busy = false;
                        Log("OBS_SHARED_TEARDOWN_VERIFIED via confirm retry — exact source cleared, gate released, busy=false");
                        return false;
                    }
                    if (p.ObsSharedStopTerminal)
                        MarkObsSharedHostTerminal("source terminal while confirming", null);
                    return false;   // confirm pending — the SAME exact-source retry timer keeps it bounded
                }
                string reason;
                bool step = p.TryStepObsSharedSourceStop(out reason);
                // re-check identity after any reentrant call the step may have pumped
                if (!ReferenceEquals(_producer, p) || !ReferenceEquals(_obsSharedStopSource, p))
                {
                    Log("OBS_SHARED_SOURCE_STEP_STALE source or coordinator identity changed — handoff dropped");
                    return false;
                }
                if (!step)
                {
                    Log("OBS_SHARED_SOURCE_STEP_PENDING reason=" + (reason ?? "none") +
                        " — source-owned 32/5000ms budget; producer/busy RETAINED");
                    return false;
                }
                ObsSharedNativeTeardownHandoff(p);   // SourceReady hands directly, p still retained
                return false;
            }
            catch (Exception ex)
            {
                _busy = true;
                MarkObsSharedHostTerminal("source_step_exception", ex);   // sticky; own timer retired inside
                return false;
            }
        }

        // Off-UI native handoff for a source-ready shared coordinator p (p RETAINED). Requires the
        // same source, !creating, !StopTerminal and SourceReady. The real-bound route additionally
        // requires ObsSharedFinalStopStateValid and the ACTUAL cached 32-byte Stop32 snapshot
        // (version 1 / flags 7 / pending 0 / reserved 0 / original epoch + positive generation from
        // the source API) — never synthesized. The distinct ObsSharedNeverBoundSourceReady is allowed
        // only by the Producer's proven-empty path. ONE VRSIM_Stop then ONE VRSIM_Destroy with the
        // actual returned dc and the actual GetState after return; NO state7/8 polling as worker-exit
        // proof (the native lifecycle owns the real join/context release), NO Task.Wait/Result/Join.
        private void ObsSharedNativeTeardownHandoff(VCFrameProducer p)
        {
            var ci = System.Globalization.CultureInfo.InvariantCulture;
            if (!ReferenceEquals(_producer, p) || !ReferenceEquals(_obsSharedStopSource, p)) return;
            if (_creating) { _exitRequested = true; return; }
            if (p.ObsSharedStopTerminal || _obsSharedHostTerminal) return;   // sticky: native never re-runs
            if (!p.ObsSharedSourceReadyForNativeTeardown) return;
            if (_obsSharedNativeTask != null) return;   // a recorded native task is NEVER started again

            int stopGen = p.ObsSharedStopBoundGeneration;
            ulong stopEpoch = p.ObsSharedStopBoundEpoch;
            if (p.ObsSharedNeverBoundSourceReady)
            {
                // proven-empty never-bound path — NO flags7 snapshot is invented for it here
                if (stopGen <= 0)
                {
                    Log("OBS_SHARED_HANDOFF_REFUSED never_bound bound generation not positive — retained");
                    return;
                }
            }
            else
            {
                if (!p.ObsSharedFinalStopStateValid || stopGen <= 0 || stopEpoch == 0UL)
                {
                    Log("OBS_SHARED_HANDOFF_REFUSED final stop snapshot absent — retained");
                    return;
                }
                var f = p.ObsSharedFinalStopState;
                if (f.structBytes != SharedGpuStopBytesV1 || f.version != SharedGpuWireVersionV1 ||
                    f.flags != 7u || f.pendingSlots != 0u || f.reserved != 0u ||
                    f.boundEpoch != stopEpoch || f.boundGeneration != (uint)stopGen)
                {
                    Log("OBS_SHARED_HANDOFF_REFUSED actual snapshot mismatch structBytes=" +
                        f.structBytes.ToString(ci) + " version=" + f.version.ToString(ci) +
                        " flags=" + f.flags.ToString(ci) + " pending=" + f.pendingSlots.ToString(ci) +
                        " reserved=" + f.reserved.ToString(ci) + " boundEpoch=" + f.boundEpoch.ToString(ci) +
                        " boundGen=" + f.boundGeneration.ToString(ci) +
                        " expectedEpoch=" + stopEpoch.ToString(ci) + " expectedGen=" + stopGen.ToString(ci) +
                        " — never synthesized; retained");
                    return;
                }
            }

            RetireProducerRetryTimer();   // retire own retry BEFORE the CAS gate
            if (Interlocked.CompareExchange(ref _teardownGate, 1, 0) != 0)
            {
                Log("OBS_SHARED_HANDOFF_SKIPPED cleanup gate already running — retained");
                return;
            }
            _busy = true;   // stays true through the whole shared teardown — no generic finally release
            Log("OBS_SHARED_NATIVE_HANDOFF begin boundGen=" + stopGen.ToString(ci) +
                " boundEpoch=" + stopEpoch.ToString(ci) +
                " neverBound=" + (p.ObsSharedNeverBoundSourceReady ? "true" : "false") +
                " one_stop_one_destroy actual_dc_state_only no_state_polling_no_ui_join");
            // ACTUAL results — written ONLY by the task body; published ONLY by the real completion
            // continuation below. The body never releases the gate and Returned is never claimed
            // before the task has ACTUALLY completed; no UI Wait/Result/Join anywhere.
            int dc = int.MinValue;
            int st = int.MinValue;
            Exception stopEx = null, destroyEx = null, stateEx = null;
            Task task = null;
            task = new Task(() =>
            {
                try { VRSIM_Stop(); } catch (Exception sex) { stopEx = sex; }
                try { dc = VRSIM_Destroy(); } catch (Exception dex) { dc = int.MinValue; destroyEx = dex; }
                try { st = VRSIM_GetState(); } catch (Exception gex) { st = int.MinValue; stateEx = gex; }
            });
            // REAL completion continuation — metadata + dispatcher post ONLY (no Stop/Destroy/SDK).
            task.ContinueWith(completedTask =>
            {
                long returnQpc = Stopwatch.GetTimestamp();
                Exception postFailure = null;
                try
                {
                    // the actual task completed — only NOW release the CAS gate and claim returned
                    Interlocked.Exchange(ref _teardownGate, 0);
                    _obsSharedNativeTaskReturned = true;
                    _disp.BeginInvoke((System.Action)(() =>
                    {
                        // re-check the exact coordinator/current source, this-generation latch AND
                        // the recorded task ownership before publishing anything
                        if (!ReferenceEquals(_producer, p) || !ReferenceEquals(_obsSharedNativeSource, p) ||
                            !ReferenceEquals(_obsSharedStopSource, p) || !ReferenceEquals(_obsSharedNativeTask, task))
                        {
                            Log("OBS_SHARED_DESTROY_RESULT_DROPPED source/task identity mismatch — stale result never clears a newer flow");
                            return;
                        }
                        _obsSharedLastNativeDestroyRc = dc;   // ACTUAL returned dc — evidence, never success by itself
                        _obsSharedNativeDestroyState = st;    // ACTUAL GetState after the task return
                        Log("OBS_SHARED_DESTROY_RETURN destroyRc=" + dc.ToString(ci) +
                            " state=" + st.ToString(ci) + " — only dc0+state0(OFF) with NO native exception may confirm");
                        if (stopEx != null || destroyEx != null || stateEx != null)
                        {
                            // ANY original native exception forbids a WholeCompleted claim even at dc0/st0
                            Exception nativeEx = stopEx;
                            if (destroyEx != null)
                                nativeEx = nativeEx == null ? destroyEx : new AggregateException(nativeEx, destroyEx);
                            if (stateEx != null)
                                nativeEx = nativeEx == null ? stateEx : new AggregateException(nativeEx, stateEx);
                            MarkObsSharedHostTerminal("native_exception — full evidence retained", nativeEx);
                            return;
                        }
                        if (dc != 0 || st != 0)
                        {
                            MarkObsSharedHostTerminal("destroy_evidence_not_clean destroyRc=" +
                                dc.ToString(ci) + " state=" + st.ToString(ci) +
                                " — actual rc/state are evidence, never completion", null);
                            return;
                        }
                        bool confirmed;
                        try { confirmed = p.ConfirmObsSharedNativeDestroyed(dc, st); }
                        catch (Exception cex)
                        {
                            MarkObsSharedHostTerminal("confirm_exception", cex);
                            return;
                        }
                        if (confirmed && p.ObsSharedWholeStopVerified)
                        {
                            _obsSharedTeardownCompleted = true;
                            if (ReferenceEquals(_producer, p)) _producer = null;   // clear the EXACT current producer/coordinator
                            _obsSharedStopSource = null;
                            _obsSharedNativeSource = null;
                            _obsSharedNativeTask = null;
                            Interlocked.Exchange(ref _teardownGate, 0);
                            RetireProducerRetryTimer();
                            _busy = false;
                            Log("OBS_SHARED_TEARDOWN_VERIFIED destroyRc=0 state=0 confirm=true wholeVerified=true — exact source cleared, gate released, busy=false");
                            return;
                        }
                        if (p.ObsSharedStopTerminal)
                        {
                            MarkObsSharedHostTerminal("source terminal while confirming", null);
                            return;
                        }
                        // pending owner confirmation: keep the task/result/source identity, the gate is
                        // already released, and retry ONLY Confirm on the UI via the existing exact-source
                        // retry timer — leaving _teardownGate at 1 here would deadlock that confirmation.
                        _obsSharedConfirmRetries = 0;
                        _obsSharedConfirmBeginQpc = returnQpc;   // 32/5000ms bound runs from the task return
                        ScheduleProducerRetry();
                    }));
                }
                catch (Exception pex) { postFailure = pex; }
                if (postFailure != null)
                {
                    // post/dispatcher failure: sticky terminal; task/result/source identity and busy
                    // retained — the result can never be published, so nothing may re-run or re-arm
                    try { MarkObsSharedHostTerminal("result_post_failure", postFailure); } catch { }
                }
            }, TaskScheduler.Default);
            // publish the recorded task + source identity BEFORE Start (registered continuation
            // included) — the completion callback can never observe an unrecorded task
            _obsSharedNativeTask = task;
            _obsSharedNativeSource = p;
            try { task.Start(TaskScheduler.Default); }
            catch (Exception sex)
            {
                if (task.Status == TaskStatus.Created)
                {
                    // definitively NEVER started — release THIS CAS; the recorded task stays retained
                    // and native cleanup can never be re-run for this source (no uncertain retry)
                    Interlocked.Exchange(ref _teardownGate, 0);
                    MarkObsSharedHostTerminal("native_task_start_failed_never_started", sex);
                    return;
                }
                // started/uncertain: NO false TaskReturned and NO early gate release — the real
                // completion continuation owns them; task/source/busy stay fully retained
                MarkObsSharedHostTerminal("native_task_start_failed_status=" + task.Status.ToString(), sex);
                return;
            }
        }

        // Sticky host terminal/error record (Host3B2A review fix): merges the FULL original
        // exceptions (Stop / Destroy / GetState / Start / post / confirm / budget) and retires this
        // coordinator's own retry timer. Once set for a generation, no Exit/Stop may re-arm the
        // retry or re-run native cleanup; producer/busy/task/source identity stay retained until a
        // real verified completion. The producer's own original exception evidence is untouched.
        private void MarkObsSharedHostTerminal(string why, Exception ex)
        {
            var full = ex ?? new InvalidOperationException("[VRSIM] OBS-SHARED host terminal — " + why);
            var prev = _obsSharedHostError;
            _obsSharedHostError = prev == null ? full : new AggregateException(prev, full);
            _obsSharedHostTerminal = true;
            RetireProducerRetryTimer();   // this timer only; never touches _producer/_busy
            Log("OBS_SHARED_HOST_TERMINAL reason=" + why +
                " — task/source/busy RETAINED; native re-run and re-arm forbidden; evidence=" +
                _obsSharedHostError.ToString());
        }

        // Native session Stop→(bounded wait)→Destroy, off the UI thread. _busy stays set
        // until cleanup completes so a retry cannot race with a half-torn-down session.
        // Interlocked cleanup gate: repeated Exit / ready-timeout cannot double Destroy.
        private void TeardownSession(string why)
        {
            _vrActive = false;
            StopSweepIfAny();
            // ---- Host3B2B OBS-SHARED routing — BEFORE the generic StopProducerIfAny / null-based
            // handoff / CAS native task, so a shared pending can NEVER fall into the generic
            // finally busy=false or the null-producer Destroy. Only A's helpers touch native, and
            // only under their own CAS.
            var cur = _producer;
            var coord = _obsSharedStopSource;
            var nat = _obsSharedNativeSource;
            bool sharedRetained = (cur != null && cur.ObsSharedSelected) ||
                coord != null || nat != null || _obsSharedNativeTask != null;
            if (sharedRetained)
            {
                if (_creating)
                {
                    // existing _creating queued-Exit policy — no sourceStop/SDK while a create runs;
                    // the create Task's own cancellation path re-enters this method afterwards.
                    _exitRequested = true;
                    _busy = true;
                    Log("TEARDOWN_SHARED_QUEUED during create reason=" + why);
                    return;
                }
                if (!_disp.CheckAccess())
                {
                    // create-Task / background caller: post-only — no UI-blocking join, no held native gate
                    _busy = true;
                    Log("TEARDOWN_SHARED_DEFERRED_TO_UI reason=" + why + " non_blocking_post=true");
                    _disp.BeginInvoke((System.Action)(() => TeardownSession(why)));
                    return;
                }
                _busy = true;   // shared pending retains busy/source — the generic release never runs
                if (coord != null && !ReferenceEquals(cur, coord))
                { Log("TEARDOWN_SHARED_MISMATCH coordinator != current producer — refs/busy RETAINED; generic path refused"); return; }
                if (nat != null && !ReferenceEquals(nat, coord))
                { Log("TEARDOWN_SHARED_MISMATCH native source != coordinator — refs/busy RETAINED"); return; }
                var p = coord ?? cur;
                if (p == null)
                { Log("TEARDOWN_SHARED_INCONSISTENT no retained shared source — refs RETAINED; no generic Destroy"); return; }
                if (_obsSharedHostTerminal || p.ObsSharedStopTerminal)
                {
                    RetireProducerRetryTimer();   // retire ONLY that timer; busy/source/evidence retained
                    Log("TEARDOWN_SHARED_TERMINAL reason=" + why +
                        " hostTerminal=" + (_obsSharedHostTerminal ? "true" : "false") +
                        " sourceTerminal=" + (p.ObsSharedStopTerminal ? "true" : "false") +
                        " — no schedule/start; Enter stays blocked");
                    return;
                }
                if (!ReferenceEquals(coord, p))
                {
                    // Not latched yet: exactly ONE first close attempt through StopProducerIfAny (its
                    // internal first source step already ran — never an extra step from this entry).
                    Log("TEARDOWN_SHARED first close attempt reason=" + why);
                    StopProducerIfAny();
                    if (p.ObsSharedWholeStopVerified) return;   // only the real destroy+Confirm clears the producer
                    if (p.ObsSharedStopTerminal || _obsSharedHostTerminal)
                    { RetireProducerRetryTimer(); return; }
                    if (_obsSharedNativeTask != null) return;   // recorded task's real completion owns publication
                    if (p.ObsSharedSourceReadyForNativeTeardown)
                        try { ObsSharedNativeTeardownHandoff(p); } catch (Exception ex) { MarkObsSharedHostTerminal("direct_handoff_exception", ex); }   // sticky terminal; busy/source/full fault RETAINED; no schedule/retry after catch
                    else
                        ScheduleProducerRetry();                // bounded exact-source UI retry
                    return;
                }
                // Already latched (e.g. manual ExitVR just ran StopProducerIfAny's ONE first step):
                // NO second source step from this same entry — progress belongs to the timer /
                // handoff / real-completion callback chain below.
                if (!ReferenceEquals(_producer, p) || !ReferenceEquals(_obsSharedStopSource, p))
                { Log("TEARDOWN_SHARED_LATCH_MISMATCH current producer/coordinator replaced during first close — refs/busy RETAINED"); return; }
                if (_obsSharedHostTerminal || p.ObsSharedStopTerminal)
                {
                    RetireProducerRetryTimer();   // retire ONLY this timer; busy/source/evidence retained
                    Log("TEARDOWN_SHARED_TERMINAL latched — no second step, no schedule; refs/busy RETAINED");
                    return;
                }
                if (_obsSharedNativeTask != null)
                {
                    // a recorded task's REAL completion continuation publishes the results; its
                    // Confirm-pending callback arms the bounded UI retry timer — never re-arm native
                    Log("TEARDOWN_SHARED_NATIVE_TASK_RECORDED real completion owns publication; no re-arm");
                    return;
                }
                if (_producerRetryTimer != null && ReferenceEquals(_producerRetrySource, p))
                { Log("TEARDOWN_SHARED_RETRY_ARMED existing exact-source tick drives the next step"); return; }
                if (p.ObsSharedSourceReadyForNativeTeardown)
                    try { ObsSharedNativeTeardownHandoff(p); } catch (Exception ex) { MarkObsSharedHostTerminal("direct_handoff_exception", ex); }   // sticky terminal; busy/source/full fault RETAINED; no schedule/retry after catch
                else
                    ScheduleProducerRetry();                // bounded exact-source UI retry — pending gets its timer
                return;
            }
            // ---- non-shared generic path (unchanged below) ----
            // P1: if the owner producer is not closed, native cleanup must NOT run early.
            // Keep _producer, keep _busy (blocks Enter), register the deferred retry, and return
            // before the Stop->Destroy gate — no CAS start, no NativeStop, never claim cleaned.
            if (!StopProducerIfAny())
            {
                _busy = true;
                Log("TEARDOWN_DEFERRED producer_unclosed reason=" + why);
                ScheduleProducerRetry();
                return;
            }
            // P1: producer closed and we are entering the native cleanup path — retire the
            // pending deferred retry (timer + bound source) BEFORE the CAS gate. When this call
            // comes FROM ProducerRetryTick the fields are already cleared, so this is a no-op.
            RetireProducerRetryTimer();
            if (Interlocked.CompareExchange(ref _teardownGate, 1, 0) != 0)
            {
                Log("TEARDOWN_SKIPPED already_running reason=" + why);
                return;
            }
            _busy = true;
            Log("TEARDOWN begin reason=" + why);
            Task.Run(() =>
            {
                try
                {
                    VRSIM_Stop();
                    for (int i = 0; i < 30; ++i)
                    {
                        int s = VRSIM_GetState();
                        if (s == 0 /*OFF*/ || s == 7 /*STOPPING*/ || s == 8 /*ERROR*/) break;
                        Thread.Sleep(100);
                    }
                    int dc = VRSIM_Destroy();
                    Log("TEARDOWN done destroyRc=" + dc + " reason=" + why);
                    WriteDiagnostics("exit");
                    // XR teardown ran regardless, but never claim FULL cleanup while a producer
                    // is retained (StopProducerIfAny keeps it on StopAndRestore failure).
                    if (_producer == null)
                        Msg("[VRSIM] XR 会话清理完成（" + why + "）");
                    else
                        Msg("[VRSIM] XR 会话已停止/销毁，但帧生产者恢复失败被保留 — 请点 Exit VR 重试（" + why + "）");
                }
                catch (Exception ex) { Log("TEARDOWN_FAIL " + ex.GetType().Name + ": " + ex.Message); }
                finally
                {
                    _vrActive = false;
                    Interlocked.Exchange(ref _teardownGate, 0);
                    _busy = false;   // gate released only after cleanup completed → Enter retry allowed
                }
            });
        }

        // ---------------- Exit VR ----------------
        private void ExitVR()
        {
            // Exit during create → QUEUE it; the create Task performs Stop/Destroy only after
            // Create has finished (Destroy must never run parallel with Create).
            if (_creating)
            {
                _exitRequested = true;
                _generation++;   // late BeginReadyWait/ReadyTick will see the stale generation
                Log("EXIT_QUEUED during create gen=" + _generation);
                Msg("[VRSIM] VR 会话正在创建中 — Exit 已排队，创建结束后立即清理");
                return;
            }
            if (!_vrActive && !_readyWaiting && _sweep == null && _producer == null)
            { Msg("[VRSIM] VR 未激活"); return; }
            _generation++;   // invalidates ready timer, sweep→replay→producer callbacks of THIS session
            Log("EXIT_VR clicked gen=" + _generation);
            Msg("[VRSIM] Exit VR …");
            StopProducerIfAny();
            StopReadyTimer();
            StopPolling();
            StopSweepIfAny();   // kills the timer + restores camera/recording — a live DispatcherTimer
                                // must never keep driving the VC camera after Exit
            TeardownSession("user exit");   // idempotent: sweep/producer already null-stopped above
            // NOTE: the monitor window STAYS OPEN on ExitVR — it shows phase "stopped" honestly.
        }

        // ---------------- 0.1.3.5 diagnostic sweep + monitor ----------------
        /// <summary>Diagnostic flow = the OLD strict sweep/replay chain, now only via explicit
        /// button, and only when NOTHING is running (no create, no session, no busy teardown).</summary>
        private void RunDiagnosticSweep()
        {
            if (_creating || _busy || _readyWaiting || _vrActive || _sweep != null || _producer != null)
            {
                Msg("[VRSIM] 当前有 VR 会话/流程在运行或清理中 — 『运行技术扫摆』仅可在空闲时启动");
                return;
            }
            EnterVRCore(true);
        }

        private void ToggleMonitor()
        {
            try
            {
                _disp.Invoke((System.Action)(() =>
                {
                    if (_monitor != null)
                    {
                        try { _monitor.Close(); }
                        catch (Exception ex) { Log("MONITOR_CLOSE_FAIL " + ex.GetType().Name + ": " + ex.Message); }
                        _monitor = null;
                        Log("MONITOR_CLOSED by button");
                        return;
                    }
                    _monitor = new VRMonitorWindow(this);
                    _monitor.Closed += (s, e) => { _monitor = null; Log("MONITOR_CLOSED"); };
                    _monitor.Show();
                    Log("MONITOR_OPENED");
                }));
            }
            catch (Exception ex)
            {
                Log("MONITOR_OPEN_FAIL " + ex.GetType().Name + ": " + ex.Message);
                Msg("[VRSIM] 监控窗口打开失败: " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        private void CloseMonitor()
        {
            try
            {
                _disp.Invoke((System.Action)(() =>
                {
                    if (_monitor == null) return;
                    try { _monitor.Close(); } catch (Exception ex) { Log("MONITOR_FORCE_CLOSE_FAIL " + ex.Message); }
                    _monitor = null;
                }));
            }
            catch (Exception ex) { Log("MONITOR_FORCE_CLOSE_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        // ---------------- polling ----------------
        private void BeginPolling()
        {
            if (_poll != null) return;
            _disp.Invoke((System.Action)(() =>
            {
                _lastState = -1;
                _poll = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
                _poll.Tick += (s, e) => PollTick();
                _poll.Start();
            }));
        }

        private void StopPolling()
        {
            if (_poll == null) return;
            try { _disp.Invoke((System.Action)(() => { _poll.Stop(); _poll = null; })); }
            catch { _poll = null; }
        }

        private void PollTick()
        {
            try
            {
                int st = VRSIM_GetState();
                if (st != _lastState)
                {
                    Log("STATE " + StateName(_lastState) + " -> " + StateName(st));
                    _lastState = st;
                    if ((st == 5 || st == 6) && !_visibleAnnounced)   // READYNESS FIX: 7/8 are NOT visible
                    {
                        _visibleAnnounced = true;
                        double secs = (DateTime.UtcNow - _enterAt).TotalSeconds;
                        Log("SESSION_VISIBLE state=" + StateName(st) + " afterSeconds=" + secs.ToString("0.0"));
                        Msg("[VRSIM] XR 会话状态 " + StateName(st) + "（" + secs.ToString("0.0") + "s）— 仅表示后端已启动，场景内容以帧门 VC_LAYER_SUCCESS 为准");
                    }
                    if (st == 8) Msg("[VRSIM] VR 错误: " + NativeError());
                }
                WriteDiagnostics("poll");
            }
            catch (Exception ex) { Log("POLL_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        internal static string StateName(int s)
        {
            switch (s)
            {
                case 0: return "OFF";
                case 1: return "STARTING";
                case 2: return "WAITING_FOR_HMD";
                case 3: return "INITIALIZING_OPENXR";
                case 4: return "SYNCHRONIZED";
                case 5: return "VISIBLE";
                case 6: return "RUNNING(FOCUSED)";
                case 7: return "STOPPING";
                case 8: return "ERROR";
                default: return "S" + s;
            }
        }

        // ---------------- evidence ----------------
        private void WriteDiagnostics(string phase)
        {
            try
            {
                var buf = new byte[8192];
                int n = VRSIM_GetDiagnostics(buf, buf.Length);
                if (n < 0) return;
                string json = Encoding.UTF8.GetString(buf, 0, n);
                var extra = new StringBuilder();
                try { var st = ReadStereoStats(); if (st.HasValue) { var v = st.Value;
                    extra.Append(",\"stereo\":{\"submitOk\":").Append(v.submitOk)
                         .Append(",\"submitFail\":").Append(v.submitFail)
                         .Append(",\"d3dUploads\":").Append(v.d3dUploads)
                         .Append(",\"vcLayerOk\":").Append(v.vcLayerOk)
                         .Append(",\"reusedFrames\":").Append(v.reusedFrames)
                         .Append(",\"latestFrameSerial\":").Append(v.latestFrameSerial)
                         .Append(",\"sceneMode\":").Append(v.sceneMode).Append("}"); } } catch { }
                try { if (_producer != null) _producer.WriteStats(extra); } catch { }
                // splice extras before the final '}'
                int close = json.LastIndexOf('}');
                json = json.Insert(close, extra.ToString());
                File.WriteAllText(_diagPath, "{\"phase\":\"" + phase + "\",\"pluginVersion\":\"" + PluginVersion + "\",\"capturedUtc\":\"" + DateTime.UtcNow.ToString("o") + "\",\"native\":" + json + "}");

                // PHASE 23/27 technical visual gate: only announce "look at the headset"
                // after 60 successful quad-layer EndFrames have actually been submitted.
                // 02A gate: VC-scene layers actually submitted (not quad counters)
                int vi = json.IndexOf("\"vcLayerOk\":", StringComparison.Ordinal);
                int smi = json.IndexOf("\"sceneMode\":", StringComparison.Ordinal);
                if (vi >= 0 && !_quadAnnounced)
                {
                    int v = 0;
                    for (int k = vi + 11; k < json.Length && json[k] >= '0' && json[k] <= '9'; ++k)
                        v = v * 10 + (json[k] - '0');
                    if (v >= 60 && smi >= 0 && json.IndexOf("\"sceneMode\":1", StringComparison.Ordinal) >= 0)
                    {
                        _quadAnnounced = true;
                        Msg("[VRSIM] VRSIM 已确认 VC 场景像素进入 OpenXR 提交纹理（VC_LAYER_SUCCESS=" + v + "）— 现在请戴头显观看");
                        Log("TECH_GATE_REACHED vcLayerOk=" + v);
                        // PHASE 17: QA readback — prove the submitted texture holds the VC scene
                        try
                        {
                            var dir = Path.Combine(_logDir, "vc-frames");
                            Directory.CreateDirectory(dir);
                            DumpSceneTexturePng(0, Path.Combine(dir, "native-vc-before-xr-left.png"));
                            DumpSceneTexturePng(1, Path.Combine(dir, "native-vc-before-xr-right.png"));
                        }
                        catch { }
                    }
                }
            }
            catch (Exception ex) { Log("DIAG_FAIL " + ex.GetType().Name + ": " + ex.Message); }
        }

        internal void Log(string line)
        {
            try { File.AppendAllText(_logPath, DateTime.UtcNow.ToString("HH:mm:ss.fff") + "Z " + line + Environment.NewLine); }
            catch { }
        }

        private void Msg(string text)
        {
            Log("MSG " + text);
            try
            {
                if (_messages == null) _messages = Caliburn.Micro.IoC.Get<IMessageService>(null);
                if (_messages != null) _messages.AppendMessage(text, MessageLevel.Info);
            }
            catch (Exception ex)
            {
                try { MessageBox.Show(text, "VRSIM"); } catch { }
                Log("MSG_FALLBACK " + ex.GetType().Name);
            }
        }
    }
}
