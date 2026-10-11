using System;
using System.Collections;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;
using Caliburn.Micro;
using VisualComponents.Create3D;
using VisualComponents.UX.Shared;
using Matrix = VisualComponents.Create3D.Matrix;

#if VRSIM_SHARED_3072 && (VRSIM_SHARED_2736 || VRSIM_SHARED_2560 || VRSIM_SHARED_2048)
#error VRSIM_SHARED_3072 is mutually exclusive with VRSIM_SHARED_2736, VRSIM_SHARED_2560 and VRSIM_SHARED_2048; define at most one
#endif
#if VRSIM_SHARED_2736 && VRSIM_SHARED_2560
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2560 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_2736 && VRSIM_SHARED_2048
#error VRSIM_SHARED_2736 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if VRSIM_SHARED_2560 && VRSIM_SHARED_2048
#error VRSIM_SHARED_2560 and VRSIM_SHARED_2048 are mutually exclusive; define at most one
#endif
#if VRSIM_SOURCE_4096_QA && !(VRSIM_NATIVE_QA && NC02_XR_QA)
#error VRSIM_SOURCE_4096_QA requires both VRSIM_NATIVE_QA and NC02_XR_QA to be defined
#endif

namespace NC02XrQA
{
    public sealed class NC01CaptureSampleV1
    {
        public int Eye;
        public byte[] Bgra, SurfaceInfo;
        public double[] ActualViewMatrix;
        public double FovY, AspectRatio, Gaze, SurfaceRenderElapsedMs;
        public bool Ortho;
        public ulong ReadbackQpcTicks, CopyQpcTicks;
        public string OwnedStageSnapshot;
        // Managed-only pair instrumentation; never part of the native surface ABI.
        public double EyeCameraMs, EyeLockWaitMs, EyeStageValidationMs, EyeAllocateMs, EyeReadbackCallMs, EyeUnlockMs, PairPrepareMs, PairFinalizeMs;
        public string PairMainBeforeSnapshot, PairMainAfterSnapshot;
    }

    public sealed class NC01MainSnapshotV1
    {
        public readonly string Serialized;
        internal NC01MainSnapshotV1(string s) { Serialized = s; }
        public bool StrictEquals(NC01MainSnapshotV1 other) { return other != null && string.Equals(Serialized, other.Serialized, StringComparison.Ordinal); }
    }

    public sealed partial class NC01HostedSceneOwner
    {
        private const int DefaultTargetWidth = 1024, DefaultTargetHeight = 1024;
        // Per-owner frozen capture target: instance readonly, set once at construction (legacy and
        // 1-arg runtime factory paths keep 1024x1024); validated BEFORE any IoC/SDK/lease consumption.
        private readonly int TargetWidth, TargetHeight;
        private const int MinTargetDimension = 2, MaxTargetDimension = 2048;
        private const long MaxTargetPayloadBytes = 32L * 1024L * 1024L;
        // Raw-pair projection validation tolerance: absolute, deliberately narrow (mirrors IsClose), recorded here
        // so offline raw-vs-world contract checks compare against a documented constant instead of guessing.
        private const double RawProjectionTolerance = 1e-6;
#if VRSIM_NATIVE_QA
        private const string HelperDllFileName = "NC01.Readback.QA.dll";
        private const string HelperDllAbsolutePath = @"C:\Users\KID\Documents\ChatGPT\项目经理\reviews\vrsim-highres-h2\nc01-candidate\NC01.Readback.QA.dll";
        private const string PreviousOwnerAssemblyAbsolutePath = @"C:\Users\KID\Documents\ChatGPT\项目经理\reviews\vrsim-highres-h2\nc01-candidate\fixed1024-candidate\NC01HostedSurfaceProbe.Run4.QA.dll";
#else
        private const string HelperDllFileName = "NC01.Readback.dll";
#endif

        [StructLayout(LayoutKind.Sequential, Pack = 8)]
        private struct NC01SurfaceInfoV1
        {
            public uint StructBytes, Version, Width, Height, Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality;
            public ulong DeviceIdentity, ReadbackQpcTicks, CopyQpcTicks;
        }

        [DllImport(HelperDllFileName, EntryPoint = "NC01_ReadSurface", CallingConvention = CallingConvention.Cdecl)]
        private static extern int NC01_ReadSurface(IntPtr borrowedSurface, uint expectedW, uint expectedH,
            [MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 4)] byte[] output, ulong capacity,
            ref NC01SurfaceInfoV1 info, uint infoCapacity);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr LoadLibraryW(string lpFileName);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr GetModuleHandleW(string lpModuleName);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern uint GetModuleFileNameW(IntPtr hModule, StringBuilder lpFilename, uint nSize);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool FreeLibrary(IntPtr hModule);

        private static readonly object ModuleLock = new object();
        private static readonly object RuntimeLeaseGate = new object();
        private static bool _runtimeLeaseTaken;
        private static NC01HostedSceneOwner _runtimeOwnerStrongRef;
        private static readonly BindingFlags Inst = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
#if VRSIM_QA_D9_BEGIN_POLL
        // Immutable reflection metadata caches. Locks are never held while invoking SDK getters.
        private struct MetadataKey : IEquatable<MetadataKey>
        {
            readonly Type _type;
            readonly string _name;
            readonly int _kind;
            readonly BindingFlags _flags;
            public MetadataKey(Type type, string name, int kind, BindingFlags flags) { _type = type; _name = name; _kind = kind; _flags = flags; }
            public override int GetHashCode() { return RuntimeHelpers.GetHashCode(_type) ^ _name.GetHashCode() ^ _kind ^ (int)_flags; }
            public override bool Equals(object obj) { return obj is MetadataKey && Equals((MetadataKey)obj); }
            public bool Equals(MetadataKey other) { return ReferenceEquals(_type, other._type) && string.Equals(_name, other._name, StringComparison.Ordinal) && _kind == other._kind && _flags == other._flags; }
        }
        private const int MaxMetadataCacheEntries = 256;
        private static readonly System.Collections.Generic.Dictionary<MetadataKey, PropertyInfo> _propertyInfoCache = new System.Collections.Generic.Dictionary<MetadataKey, PropertyInfo>();
        private static readonly System.Collections.Generic.Dictionary<MetadataKey, FieldInfo> _fieldInfoCache = new System.Collections.Generic.Dictionary<MetadataKey, FieldInfo>();
        private static readonly object _propertyInfoCacheLock = new object();
        private static readonly object _fieldInfoCacheLock = new object();
        private static MethodInfo _cachedGetSectionClosedDisplay;
        private static readonly object _getSectionCacheLock = new object();
        private static PropertyInfo FindPropertyCached(Type type, string name)
        {
            MetadataKey key = new MetadataKey(type, name, 0, Inst);
            PropertyInfo cached;
            lock (_propertyInfoCacheLock) { if (_propertyInfoCache.TryGetValue(key, out cached)) return cached; }
            PropertyInfo result = null;
            for (Type t = type; t != null; t = t.BaseType) { result = t.GetProperty(name, Inst); if (result != null) break; }
            if (result != null) { lock (_propertyInfoCacheLock) { if (_propertyInfoCache.Count < MaxMetadataCacheEntries) _propertyInfoCache[key] = result; } }
            return result;
        }
        private static FieldInfo FindFieldCached(Type type, string name)
        {
            MetadataKey key = new MetadataKey(type, name, 1, Inst);
            FieldInfo cached;
            lock (_fieldInfoCacheLock) { if (_fieldInfoCache.TryGetValue(key, out cached)) return cached; }
            FieldInfo result = null;
            for (Type t = type; t != null; t = t.BaseType) { result = t.GetField(name, Inst); if (result != null) break; }
            lock (_fieldInfoCacheLock) { if (_fieldInfoCache.Count < MaxMetadataCacheEntries) _fieldInfoCache[key] = result; }
            return result;
        }
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_OWNER_PHASE_CPU_WALL
        // CPU-wall carrier: numbers only. Never store SDK receivers, sections, elements, property values, strings, or exceptions here.
        private sealed class NC01ConfiguredQualityCallsCpuCacheV1
        {
            public long SchemaVersion;
            public long Frequency;
            public long OwnerObjectIdentityHash;
            public long Seq;
            public long ReturnedTotal;
            public long CompleteCount;
            public long InvalidCount;
            public long OverflowFlag;
            public long ResolveServiceSumTicks;
            public long GetSectionSumTicks;
            public long DisplayAndGettersSumTicks;
            public long TotalSumTicks;
        }
        private NC01ConfiguredQualityCallsCpuCacheV1 _qaConfiguredQualityCallsCpuCacheV1;

        private static long QaConfiguredQualityWallTimestampOrZero()
        {
            try { return System.Diagnostics.Stopwatch.GetTimestamp(); }
            catch (Exception) { return 0L; }
        }

        // Wraps only the new timing/carrier code; never the original SDK calls.
        private void CommitConfiguredQualityCpuWallSample(long t0, long t1, long t2, long t3)
        {
            try
            {
                bool complete = false;
                long resolveTicks = 0L, sectionTicks = 0L, displayTicks = 0L, totalTicks = 0L;
                try
                {
                    if (t0 > 0L && t1 >= t0 && t2 >= t1 && t3 >= t2)
                    {
                        long seg1 = checked(t1 - t0);
                        long seg2 = checked(t2 - t1);
                        long seg3 = checked(t3 - t2);
                        long total = checked(t3 - t0);
                        if (checked(checked(seg1 + seg2) + seg3) == total)
                        {
                            resolveTicks = seg1; sectionTicks = seg2; displayTicks = seg3; totalTicks = total;
                            complete = true;
                        }
                    }
                }
                catch (OverflowException) { complete = false; }
                NC01ConfiguredQualityCallsCpuCacheV1 cache = _qaConfiguredQualityCallsCpuCacheV1;
                if (cache == null)
                {
                    NC01ConfiguredQualityCallsCpuCacheV1 fresh = new NC01ConfiguredQualityCallsCpuCacheV1();
                    fresh.SchemaVersion = 1L;
                    fresh.Frequency = System.Diagnostics.Stopwatch.Frequency;
                    fresh.OwnerObjectIdentityHash = RuntimeHelpers.GetHashCode(this);
                    System.Threading.Interlocked.CompareExchange(ref _qaConfiguredQualityCallsCpuCacheV1, fresh, null);
                    cache = _qaConfiguredQualityCallsCpuCacheV1;
                    if (cache == null) return;
                }
                lock (cache)
                {
                    long evenSeq = cache.Seq & ~1L;
                    cache.Seq |= 1L;
                    try
                    {
                        long nextReturned = 0L, nextComplete = 0L, nextInvalid = 0L;
                        long nextResolve = 0L, nextSection = 0L, nextDisplay = 0L, nextTotal = 0L, nextSeq = 0L;
                        bool computed = false;
                        try
                        {
                            nextReturned = checked(cache.ReturnedTotal + 1L);
                            if (complete)
                            {
                                nextComplete = checked(cache.CompleteCount + 1L);
                                nextInvalid = cache.InvalidCount;
                                nextResolve = checked(cache.ResolveServiceSumTicks + resolveTicks);
                                nextSection = checked(cache.GetSectionSumTicks + sectionTicks);
                                nextDisplay = checked(cache.DisplayAndGettersSumTicks + displayTicks);
                                nextTotal = checked(cache.TotalSumTicks + totalTicks);
                            }
                            else
                            {
                                nextComplete = cache.CompleteCount;
                                nextInvalid = checked(cache.InvalidCount + 1L);
                                nextResolve = cache.ResolveServiceSumTicks;
                                nextSection = cache.GetSectionSumTicks;
                                nextDisplay = cache.DisplayAndGettersSumTicks;
                                nextTotal = cache.TotalSumTicks;
                            }
                            nextSeq = checked(nextReturned * 2L);
                            computed = true;
                        }
                        catch (OverflowException) { computed = false; }
                        if (computed)
                        {
                            cache.ReturnedTotal = nextReturned;
                            cache.CompleteCount = nextComplete;
                            cache.InvalidCount = nextInvalid;
                            cache.ResolveServiceSumTicks = nextResolve;
                            cache.GetSectionSumTicks = nextSection;
                            cache.DisplayAndGettersSumTicks = nextDisplay;
                            cache.TotalSumTicks = nextTotal;
                            evenSeq = nextSeq;
                        }
                        else
                        {
                            cache.OverflowFlag = 1L;
                        }
                    }
                    finally
                    {
                        cache.Seq = evenSeq;
                    }
                }
            }
            catch (Exception) { }
        }
#endif
#endif
        private static bool _moduleChecked;
        private readonly IApplication _app;
        private readonly bool _runtimeBackend;
        private readonly IViewport _mainVp;
        private readonly IWindow3D _mainWindow;
        private readonly IDockAwareWindowManager _dock;
        private readonly Dispatcher _ui;
        private readonly IWindow3D[] _eyes = new IWindow3D[2];
        private readonly IViewport[] _vms = new IViewport[2];
        private readonly object[] _views = new object[2];
        private readonly D3DImage[] _images = new D3DImage[2];
        private readonly string[] _layoutEvidence = new string[2];
        private readonly string[] _eyeStageSnapshots = new string[2];
        private readonly string[] _eyeStagePending = new string[2];
        private readonly bool[] _eyeDisplayCopied = new bool[2];
        private string _sessionId, _disabledReason, _historicalMainSnapshot, _adoptedMainSnapshot;
        private double _scaleX = 1.0, _scaleY = 1.0;
        // Runtime generation/admission state (all pure managed, no SDK):
        // _boundGeneration is 0 until the first accepted BeginHosting(generation) or RequestStop(generation) atomically binds
        // a positive value, which then never changes; _admissionOpen is 1 only while a fully successful runtime Begin holds the
        // gate open; _stopRequested is 1 permanently once RequestStop is accepted and is the authoritative close signal for
        // every in-flight multi-group transaction; _runtimeCallsBusy marks an entered Begin/Ready/raw-pair logical call.
        // No individual int is trusted to make a whole admission/stop decision atomic: every multi-field decision over the
        // four generation fields below is taken or flipped exclusively under _gateLock (short managed lock only —
        // never held across SDK, IoC, WPF, Matrix indexer, shouldStop callback or Dispose work).
        private int _hostingState, _captureBusy;
        private int _boundGeneration, _admissionOpen, _stopRequested, _runtimeCallsBusy;
        private readonly object _gateLock = new object();
        private bool _captureDisabled;
        private NC01MainSnapshotV1 _baseline, _lastBefore, _lastAfter;
        private object _borrowedOwner;
        // Runtime-only strong identity bindings captured at construction; verified by reference, never by active-focus hash.
        private object _runtimeWorldStrongRef;
        private ICamera _runtimeMainCameraStrongRef;
        private double _runtimeMainWidth, _runtimeMainHeight;
        private string _runtimeQualityFingerprint, _qualityPendingFingerprint, _runtimeMainCameraPointerEvidence;
        private string[] _frozenQualitySubset;
        private int _qualityGeneration;

        public NC01HostedSceneOwner(IApplication app) : this(app, false, DefaultTargetWidth, DefaultTargetHeight) { }

        private NC01HostedSceneOwner(IApplication app, bool runtimeBackend, int targetWidth, int targetHeight)
        {
            if (app == null) throw new ArgumentNullException("app");
            // Pure managed target validation FIRST: before the UI-thread gate, any IoC/SIM SDK getter
            // and any lease effect, so an invalid size can never consume runtime state.
            ValidateTargetDimensions(targetWidth, targetHeight);
            TargetWidth = targetWidth;
            TargetHeight = targetHeight;
            // Runtime path: thread gate and _ui binding happen BEFORE any IoC/SIM SDK getter and before EnsureHelperLoaded.
            if (runtimeBackend)
            {
                Application wpfApp = System.Windows.Application.Current;
                if (wpfApp == null) throw new InvalidOperationException("NC01_RUNTIME_UI_APPLICATION_NULL");
                Dispatcher ui = wpfApp.Dispatcher;
                if (ui == null || !ui.CheckAccess()) throw new InvalidOperationException("NC01_RUNTIME_UI_THREAD_REQUIRED:checkAccess=" + Bool(ui != null && ui.CheckAccess()));
                _ui = ui;
            }
            _app = app;
            _runtimeBackend = runtimeBackend;
            _mainVp = IoC.Get<IViewport>(null);
            if (_mainVp == null) throw new InvalidOperationException("NC01_IOC_NO_VIEWPORT");
            _mainWindow = _mainVp.Window;
            if (_mainWindow == null) throw new InvalidOperationException("NC01_NO_MAIN_WINDOW");
            if (runtimeBackend)
            {
                object world = app.World;
                if (world == null) throw new InvalidOperationException("NC01_RUNTIME_WORLD_NULL");
                _runtimeWorldStrongRef = world;
                ICamera mainCamera = _mainWindow.Camera;
                if (mainCamera == null) throw new InvalidOperationException("NC01_RUNTIME_MAIN_CAMERA_NULL");
                _runtimeMainCameraStrongRef = mainCamera;
                // Public IWrapper.Pointer is captured for drift diagnostics only; identity is bound by reference, never inferred from it.
                object mainCameraPointer = ReflectTryGetter(mainCamera, "Pointer");
                _runtimeMainCameraPointerEvidence = mainCameraPointer is IntPtr
                    ? ((IntPtr)mainCameraPointer).ToInt64().ToString("x16", CultureInfo.InvariantCulture) : "<unreadable>";
                _runtimeMainWidth = _mainWindow.Width;
                _runtimeMainHeight = _mainWindow.Height;
            }
            _dock = IoC.Get<IDockAwareWindowManager>(null);
            if (_dock == null) throw new InvalidOperationException("NC01_IOC_NO_DOCKAWARE_WM");
            if (!runtimeBackend) _ui = System.Windows.Application.Current.Dispatcher;
            EnsureHelperLoaded();
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
            // Cold one-time companion allocation; the accepted hot path only reads the existing companion or null.
            SplitQaMainSnapshotInnerEnsureQa(this, _qaOwnerPhaseCpuWallCacheQa);
#endif
        }

        // Fresh-owned-SIM-process runtime bootstrap: exactly one lease per process, UI thread required, construct-and-bind only.
        // Never shows windows, never waits for Ready, never captures; the producer calls BeginHosting afterwards. The lease is
        // consumed even on partial construction failure and the created owner is held statically for the process lifetime.
        // Legacy factory: keeps the frozen 1024x1024 default by delegating to the sized overload.
        public static NC01HostedSceneOwner CreateRuntimeOwner(IApplication app)
        {
            return CreateRuntimeOwner(app, DefaultTargetWidth, DefaultTargetHeight);
        }

        // Sized runtime factory: the per-owner target is frozen here (never re-read from CaptureProfile); the
        // 2..2048 / checked BGRA<=32MiB validation is pure managed and runs BEFORE the single runtime lease and
        // before the UI-thread gate, so an invalid size never consumes the lease. All other discipline is unchanged.
        public static NC01HostedSceneOwner CreateRuntimeOwner(IApplication app, int width, int height)
        {
            if (app == null) throw new ArgumentNullException("app");
            ValidateTargetDimensions(width, height);
            // Thread gate BEFORE the lease: a wrong-thread caller must not consume the single runtime lease.
            Application gateApp = System.Windows.Application.Current;
            if (gateApp == null) throw new InvalidOperationException("NC01_RUNTIME_UI_APPLICATION_NULL");
            Dispatcher gateUi = gateApp.Dispatcher;
            if (gateUi == null || !gateUi.CheckAccess()) throw new InvalidOperationException("NC01_RUNTIME_UI_THREAD_REQUIRED:checkAccess=" + Bool(gateUi != null && gateUi.CheckAccess()));
            lock (RuntimeLeaseGate)
            {
                if (_runtimeLeaseTaken) throw new InvalidOperationException("NC01_RUNTIME_LEASE_ALREADY_GRANTED");
                // Reserve before constructing: a failed attempt must not reopen the lease nor allow a second runtime owner.
                _runtimeLeaseTaken = true;
            }
            NC01HostedSceneOwner owner = new NC01HostedSceneOwner(app, true, width, height);
            owner.VerifyAccess();
            lock (RuntimeLeaseGate) { _runtimeOwnerStrongRef = owner; }
            return owner;
        }

        // Pure managed per-owner target validation: dimensions within 2..2048 inclusive and the BGRA buffer
        // size (checked width*height*4) within 32 MiB. No SDK, IoC or WPF access. The 2560 compile variant
        // ONLY additionally accepts the exact 2560x2560 square — MaxTargetDimension itself stays 2048 so
        // no other large size or 2560-adjacent rectangle is relaxed, and the 32 MiB gate below is unchanged.
        private static void ValidateTargetDimensions(int width, int height)
        {
#if VRSIM_SOURCE_4096_QA
            if ((width < MinTargetDimension || width > MaxTargetDimension || height < MinTargetDimension || height > MaxTargetDimension)
                && !(width == 4096 && height == 4096))
                throw new InvalidOperationException("NC01_TARGET_DIMENSION_INVALID:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height) + "|rule=2-to-2048-inclusive-or-exact-4096x4096-only");
#elif VRSIM_SHARED_3072
            if ((width < MinTargetDimension || width > MaxTargetDimension || height < MinTargetDimension || height > MaxTargetDimension)
                && !(width == 3072 && height == 3072))
                throw new InvalidOperationException("NC01_TARGET_DIMENSION_INVALID:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height) + "|rule=2-to-2048-inclusive-or-exact-3072x3072-only");
#elif VRSIM_SHARED_2736
            if ((width < MinTargetDimension || width > MaxTargetDimension || height < MinTargetDimension || height > MaxTargetDimension)
                && !(width == 2736 && height == 2736))
                throw new InvalidOperationException("NC01_TARGET_DIMENSION_INVALID:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height) + "|rule=2-to-2048-inclusive-or-exact-2736x2736-only");
#elif VRSIM_SHARED_2560
            if ((width < MinTargetDimension || width > MaxTargetDimension || height < MinTargetDimension || height > MaxTargetDimension)
                && !(width == 2560 && height == 2560))
                throw new InvalidOperationException("NC01_TARGET_DIMENSION_INVALID:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height) + "|rule=2-to-2048-inclusive-or-exact-2560x2560-only");
#else
            if (width < MinTargetDimension || width > MaxTargetDimension || height < MinTargetDimension || height > MaxTargetDimension)
                throw new InvalidOperationException("NC01_TARGET_DIMENSION_INVALID:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height) + "|rule=2-to-2048-inclusive");
#endif
            long bgraPayload = checked((long)width * (long)height * 4L);
#if VRSIM_SOURCE_4096_QA
            // GPU-surface equivalent footprint exception ONLY: the exact 4096x4096 square whose checked BGRA
            // equivalent is 67108864 bytes (64 MiB) may pass this gate for the frozen GPU seam. This is NOT
            // a CPU buffer allowance; every other over-32MiB geometry still throws, and builds without
            // VRSIM_SOURCE_4096_QA keep the original gate verbatim below.
            if (bgraPayload > MaxTargetPayloadBytes
                && !(width == 4096 && height == 4096 && bgraPayload == 67108864L))
                throw new InvalidOperationException("NC01_TARGET_BGRA_PAYLOAD_OVER_32MIB:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height)
                    + "|bgraBytes=" + bgraPayload.ToString(CultureInfo.InvariantCulture));
#elif VRSIM_SHARED_3072
            // GPU-surface equivalent footprint exception ONLY: the exact 3072x3072 square whose checked BGRA
            // equivalent is 37748736 bytes (36 MiB) may pass this gate for the frozen GPU seam. This is NOT
            // a CPU buffer allowance; every other over-32MiB geometry still throws, and builds without
            // VRSIM_SHARED_3072 keep the original gate verbatim below.
            if (bgraPayload > MaxTargetPayloadBytes
                && !(width == 3072 && height == 3072 && bgraPayload == 37748736L))
                throw new InvalidOperationException("NC01_TARGET_BGRA_PAYLOAD_OVER_32MIB:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height)
                    + "|bgraBytes=" + bgraPayload.ToString(CultureInfo.InvariantCulture));
#else
            if (bgraPayload > MaxTargetPayloadBytes)
                throw new InvalidOperationException("NC01_TARGET_BGRA_PAYLOAD_OVER_32MIB:width=" + FormatInvariant(width)
                    + "|height=" + FormatInvariant(height)
                    + "|bgraBytes=" + bgraPayload.ToString(CultureInfo.InvariantCulture));
#endif
        }

        public bool RuntimeBackend { get { return _runtimeBackend; } }

        // Pure managed getters: expose the sticky failure reason and the managed quality generation (0 pending, 1 frozen).
        public string FailureReason { get { return _disabledReason; } }
        public int QualityGeneration { get { return _qualityGeneration; } }

        // Runtime admission surface. Every getter here is managed-only: no SDK getter, no external callback, no Dispose,
        // and each returns ONE consistent snapshot of the four generation fields taken under the short gate lock.
        public int BoundGeneration { get { lock (_gateLock) { return Volatile.Read(ref _boundGeneration); } } }

        // The gate is open only between a fully successful BeginHosting(generation) and the first accepted RequestStop or
        // a runtime fault close; a close is permanent and the lease is never re-granted or re-bound.
        public bool AdmissionOpen { get { lock (_gateLock) { return Volatile.Read(ref _admissionOpen) != 0 && Volatile.Read(ref _stopRequested) == 0; } } }

        // Honest busy: any entered runtime BeginHosting/Ready/raw-pair logical call OR the legacy capture/pair busy guard.
        public bool CaptureBusy { get { lock (_gateLock) { return Volatile.Read(ref _runtimeCallsBusy) != 0 || Volatile.Read(ref _captureBusy) != 0; } } }

        // Stop is verified for THIS owner's logical SDK transactions only: the gate is permanently closed (stopRequested)
        // AND no entered Begin/Ready/raw-pair/capture logical call (including required D3DImage Unlock windows) is still
        // registered — one consistent lock-held snapshot, never separate atomics. A fault close alone (DisableCaptures)
        // is not a stop and never reports verified here. It does NOT claim the whole SDK is silent — the official
        // CompositionTarget.Rendering and main-viewport rendering continue independently.
        public bool StopVerified { get { lock (_gateLock) { return Volatile.Read(ref _stopRequested) != 0 && Volatile.Read(ref _runtimeCallsBusy) == 0 && Volatile.Read(ref _captureBusy) == 0 && _stagingHelperDrained && Volatile.Read(ref _stagingPendingCount) == 0; } } }

#if VRSIM_NATIVE_QA
        // Managed adapter bootstrap: adopts already-hosted SDK window/view references from a disk-gated previous owner without creating windows or re-hosting.
        public static NC01HostedSceneOwner FromIdleOwner(IApplication app, object previous)
        {
            NC01HostedSceneOwner owner = new NC01HostedSceneOwner(app);
            owner.VerifyAccess();
            if (previous == null) throw new InvalidOperationException("NC01_PREVIOUS_OWNER_NULL");
            Type previousType = previous.GetType();
            if (previousType.FullName != "NC01.NC01HostedSceneOwner") throw new InvalidOperationException("NC01_PREVIOUS_OWNER_TYPE_UNEXPECTED:actualtype=" + previousType.FullName);
            string previousLocation;
            try { previousLocation = previousType.Assembly.Location; }
            catch (Exception ex) { throw new InvalidOperationException("NC01_PREVIOUS_OWNER_ASSEMBLY_LOCATION_UNREADABLE:originaltype=" + ex.GetType().FullName + "|originalmessage=" + ex.Message); }
            if (string.IsNullOrEmpty(previousLocation) || !Path.IsPathRooted(previousLocation)) throw new InvalidOperationException("NC01_PREVIOUS_OWNER_ASSEMBLY_NOT_DISK_BACKED:location=" + FormatInvariant(previousLocation));
            string previousFull;
            try { previousFull = Path.GetFullPath(previousLocation); }
            catch (Exception ex) { throw new InvalidOperationException("NC01_PREVIOUS_OWNER_ASSEMBLY_PATH_UNRESOLVABLE:originaltype=" + ex.GetType().FullName + "|originalmessage=" + ex.Message); }
            if (!string.Equals(previousFull, PreviousOwnerAssemblyAbsolutePath, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("NC01_PREVIOUS_OWNER_ASSEMBLY_PATH_MISMATCH:expected=" + PreviousOwnerAssemblyAbsolutePath + "|actual=" + previousFull);

            if (!ReferenceEquals(ReflectRequireField(previous, "_app"), owner._app)) throw new InvalidOperationException("NC01_PREVIOUS_APP_REF_MISMATCH");
            if (!ReferenceEquals(ReflectRequireField(previous, "_mainVp"), owner._mainVp)) throw new InvalidOperationException("NC01_PREVIOUS_MAIN_VIEWPORT_REF_MISMATCH");
            if (!ReferenceEquals(ReflectRequireField(previous, "_mainWindow"), owner._mainWindow)) throw new InvalidOperationException("NC01_PREVIOUS_MAIN_WINDOW_REF_MISMATCH");
            if (!ReferenceEquals(ReflectRequireField(previous, "_ui"), owner._ui)) throw new InvalidOperationException("NC01_PREVIOUS_DISPATCHER_REF_MISMATCH");

            object previousHostingState = ReflectRequireField(previous, "_hostingState");
            if (!(previousHostingState is int) || (int)previousHostingState != 2) throw new InvalidOperationException("NC01_PREVIOUS_NOT_IDLE_HOSTED:hostingState=" + FormatInvariant(previousHostingState));
            object previousCaptureBusy = ReflectRequireField(previous, "_captureBusy");
            if (!(previousCaptureBusy is int) || (int)previousCaptureBusy != 0) throw new InvalidOperationException("NC01_PREVIOUS_CAPTURE_BUSY:captureBusy=" + FormatInvariant(previousCaptureBusy));
            object previousCaptureDisabled = ReflectRequireField(previous, "_captureDisabled");
            if (!(previousCaptureDisabled is bool) || (bool)previousCaptureDisabled) throw new InvalidOperationException("NC01_PREVIOUS_CAPTURE_DISABLED:captureDisabled=" + FormatInvariant(previousCaptureDisabled));

            CopyIdleArray(previous, "_eyes", owner._eyes);
            CopyIdleArray(previous, "_vms", owner._vms);
            CopyIdleArray(previous, "_views", owner._views);
            CopyIdleArray(previous, "_images", owner._images);
            CopyIdleArray(previous, "_layoutEvidence", owner._layoutEvidence);
            CopyIdleArray(previous, "_eyeStageSnapshots", owner._eyeStageSnapshots);
            CopyIdleArray(previous, "_eyeStagePending", owner._eyeStagePending);
            CopyIdleArray(previous, "_eyeDisplayCopied", owner._eyeDisplayCopied);
            RequireIdleElements(owner._eyes, "_eyes");
            RequireIdleElements(owner._vms, "_vms");
            RequireIdleElements(owner._views, "_views");
            RequireIdleElements(owner._images, "_images");
            RequireIdleElements(owner._eyeStageSnapshots, "_eyeStageSnapshots");
            for (int eye = 0; eye < 2; eye++)
                if (!owner._eyeDisplayCopied[eye]) throw new InvalidOperationException("NC01_PREVIOUS_DISPLAY_NOT_COPIED:eye=" + eye.ToString(CultureInfo.InvariantCulture));

            object previousScaleX = ReflectRequireField(previous, "_scaleX");
            object previousScaleY = ReflectRequireField(previous, "_scaleY");
            if (!(previousScaleX is double) || !(previousScaleY is double)
                || !IsFinitePositive((double)previousScaleX) || !IsFinitePositive((double)previousScaleY))
                throw new InvalidOperationException("NC01_PREVIOUS_SCALE_INVALID:scaleX=" + FormatInvariant(previousScaleX) + "|scaleY=" + FormatInvariant(previousScaleY));
            owner._scaleX = (double)previousScaleX;
            owner._scaleY = (double)previousScaleY;

            object previousBaseline = ReflectRequireField(previous, "_baseline");
            if (previousBaseline == null) throw new InvalidOperationException("NC01_PREVIOUS_BASELINE_NULL");
            Type baselineType = previousBaseline.GetType();
            if (baselineType.FullName != "NC01.NC01MainSnapshotV1") throw new InvalidOperationException("NC01_PREVIOUS_BASELINE_TYPE_UNEXPECTED:actualtype=" + baselineType.FullName);
            FieldInfo serializedField = null;
            for (Type t = baselineType; t != null && serializedField == null; t = t.BaseType) serializedField = t.GetField("Serialized", Inst);
            if (serializedField == null) throw new InvalidOperationException("NC01_PREVIOUS_BASELINE_SERIALIZED_FIELD_MISSING");
            object serialized = serializedField.GetValue(previousBaseline);
            if (!(serialized is string) || ((string)serialized).Length == 0) throw new InvalidOperationException("NC01_PREVIOUS_BASELINE_SERIALIZED_EMPTY");
            // A desktop camera legitimately changes BETWEEN runs: the legacy serialized snapshot is kept as historical reference,
            // the CURRENT main snapshot is adopted after strict non-camera validation; no main camera state is reset or written.
            string historicalSerialized = (string)serialized;
            NC01MainSnapshotV1 currentSnapshot = owner.MainSnapshot();
            string[] historicalFields = ParseMainSnapshotOrdered(historicalSerialized, "historical");
            string[] currentFields = ParseMainSnapshotOrdered(currentSnapshot.Serialized, "current");
            for (int field = 0; field < SnapshotFieldOrder.Length; field++)
            {
                if (string.Equals(historicalFields[field], currentFields[field], StringComparison.Ordinal)) continue;
                if (CrossRunCameraFieldMask[field]) continue;
                throw new InvalidOperationException("NC01_FROM_IDLE_NON_CAMERA_FIELD_DRIFT:field=" + SnapshotFieldOrder[field]
                    + "|historical=" + TruncateForDiagnostic(historicalFields[field])
                    + "|current=" + TruncateForDiagnostic(currentFields[field]));
            }
            owner._baseline = currentSnapshot;
            if (!currentSnapshot.StrictEquals(owner.MainSnapshot())) throw new InvalidOperationException("NC01_FROM_IDLE_BASELINE_UNSTABLE");
            owner._historicalMainSnapshot = historicalSerialized;
            owner._adoptedMainSnapshot = currentSnapshot.Serialized;
            owner._borrowedOwner = previous;
            owner._hostingState = 2;
            return owner;
        }

        public string HistoricalMainSnapshot { get { return _historicalMainSnapshot; } }
        public string AdoptedMainSnapshot { get { return _adoptedMainSnapshot; } }

        private static void CopyIdleArray<T>(object previous, string fieldName, T[] destination)
        {
            object raw = ReflectRequireField(previous, fieldName);
            if (raw == null) throw new InvalidOperationException("NC01_PREVIOUS_ARRAY_NULL:" + fieldName);
            if (raw.GetType() != typeof(T[])) throw new InvalidOperationException("NC01_PREVIOUS_ARRAY_TYPE_UNEXPECTED:" + fieldName + "|actualtype=" + raw.GetType().FullName);
            T[] source = raw as T[];
            if (source == null) throw new InvalidOperationException("NC01_PREVIOUS_ARRAY_CAST_FAILED:" + fieldName);
            if (source.Length != 2) throw new InvalidOperationException("NC01_PREVIOUS_ARRAY_LENGTH_UNEXPECTED:" + fieldName + "|actual=" + source.Length.ToString(CultureInfo.InvariantCulture));
            Array.Copy(source, destination, 2);
        }

        private static void RequireIdleElements<T>(T[] values, string fieldName) where T : class
        {
            if (values[0] == null || values[1] == null) throw new InvalidOperationException("NC01_PREVIOUS_ELEMENT_NULL:" + fieldName);
        }
#endif

        public void BeginHosting()
        {
            VerifyAccess();
            // Runtime legacy overload is not an escape hatch: it only forwards to the already-bound generation and a
            // runtime lease with no bound generation cannot Begin through this entry at all. Non-runtime is unchanged.
            if (_runtimeBackend)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound <= 0) throw new InvalidOperationException("NC01_RUNTIME_GENERATION_NOT_BOUND:beginHostingLegacy");
                BeginHosting(bound, RuntimeNeverStop);
                return;
            }
            BeginHostingLegacyCore();
        }

        // Runtime generation Begin. The first accepted call atomically binds the positive generation (once bound it never
        // changes) and, ONLY on full success of every group, opens the admission gate. RequestStop permanently closes the
        // gate, so a later same-generation Begin can never revive this lease. Per eye (create+configure+Show) is one group;
        // the prepare snapshot/DPI block is one group. A stop landing mid-group lets the entered group finish its required
        // SDK work; only the NEXT group is refused. Created window/viewport references were already stored in the owner's
        // strong arrays before any refusal — nothing official is dropped.
        public void BeginHosting(int generation, Func<bool> shouldStop)
        {
            if (shouldStop == null) throw new ArgumentNullException("shouldStop");
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            VerifyAccess();
            if (!_runtimeBackend) throw new InvalidOperationException("NC01_BEGIN_GENERATION_RUNTIME_ONLY:runtimeBackend=0");
            // Bind-once claim, foreign-generation refusal, permanent stop refusal, fault refusal and the logical busy
            // registration are decided from ONE managed snapshot under the gate lock. Begin does not require admission
            // to be already open (this call is what opens it), but a stop or fault close refuses before any SDK work.
            // UI re-entry is refused by the busy flag with a plain throw — no sticky FailureReason write.
            lock (_gateLock)
            {
                Interlocked.CompareExchange(ref _boundGeneration, generation, 0);
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) throw new InvalidOperationException("NC01_BEGIN_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _stopRequested) != 0) throw new OperationCanceledException("NC01_BEGIN_AFTER_STOP:generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _hostingState) == 3 || _captureDisabled) throw new InvalidOperationException("NC01_BEGIN_FAULT_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture)
                    + "|hostingState=" + Volatile.Read(ref _hostingState).ToString(CultureInfo.InvariantCulture) + "|captureDisabled=" + Bool(_captureDisabled));
                if (_runtimeCallsBusy != 0) throw new InvalidOperationException("NC01_RUNTIME_CALL_BUSY:beginHosting|generation=" + generation.ToString(CultureInfo.InvariantCulture));
                _runtimeCallsBusy = 1;
            }
            // The shouldStop callback and every SDK group run OUTSIDE the gate lock; busy is returned under the same lock.
            try { BeginHostingRuntimeCore(generation, shouldStop); }
            finally { lock (_gateLock) { Volatile.Write(ref _runtimeCallsBusy, 0); } }
        }

        private void BeginHostingRuntimeCore(int generation, Func<bool> shouldStop)
        {
            if (Interlocked.CompareExchange(ref _hostingState, 1, 0) != 0) throw new InvalidOperationException("NC01_ALREADY_HOSTING");
            try
            {
                RequireRuntimeGroup(generation, shouldStop, "NC01_BEGIN_GROUP_PREPARE", false, "NC02_BEGIN_STOP_REQUESTED");
                _baseline = MainSnapshotCore();
                _sessionId = Guid.NewGuid().ToString("N");
                Window mainWpf = Application.Current.MainWindow;
                if (mainWpf == null) throw new InvalidOperationException("NC01_NO_MAIN_WPF_WINDOW");
                PresentationSource src = PresentationSource.FromVisual(mainWpf);
                if (src == null || src.CompositionTarget == null) throw new InvalidOperationException("NC01_NO_COMPOSITION_TARGET");
                System.Windows.Media.Matrix device = src.CompositionTarget.TransformToDevice;
                if (!IsFinitePositive(device.M11) || !IsFinitePositive(device.M22)) throw new InvalidOperationException("NC01_DPI_SCALE_INVALID");
                _scaleX = device.M11; _scaleY = device.M22;
                for (int eye = 0; eye < 2; eye++)
                {
                    RequireRuntimeGroup(generation, shouldStop, "NC01_BEGIN_GROUP_EYE" + eye.ToString(CultureInfo.InvariantCulture), false, "NC02_BEGIN_STOP_REQUESTED");
                    string eyeName = eye == 0 ? "LEFT" : "RIGHT";
                    IWindow3D window = _app.Create3DWindow(_app.World);
                    IViewport vm = _mainVp.CreateViewport(window);
                    vm.DisplayName = "NC01_STEREO_" + eyeName;
                    vm.IsSimulationControlVisible = false; vm.IsToolBarVisible = false; vm.IsCubeVisible = false;
                    // Field setters are no-ops until the SDK wires the eye's own traverser; copy is deferred to TryEyeReady behind target-RenderStages evidence.
                    // Direct3DViewportView is only populated after OnViewLoaded; reading it here would fail.
                    _eyes[eye] = window; _vms[eye] = vm;
                    _dock.ShowFloatingWindow("NC01_H2_" + eyeName + "_" + _sessionId, vm, new Size(TargetWidth / _scaleX + 40.0, TargetHeight / _scaleY + 60.0), null, false, false, true);
                    // Post-group managed recheck: a stop that landed during this eye's group refuses the next group instead
                    // of starting the other eye. Already-created official resources stay in the strong arrays.
                    RequireRuntimeGroup(generation, shouldStop, "NC01_BEGIN_GROUP_EYE" + eye.ToString(CultureInfo.InvariantCulture) + "_POST", false, "NC02_BEGIN_STOP_REQUESTED");
                }
                // Begin commit: the external shouldStop runs OUTSIDE the gate lock first and may re-enter RequestStop
                // while still returning false; the locked re-verification below refuses regardless. Re-verification of
                // the original generation, permanent stop, fault close and busy plus the hostingState/admissionOpen
                // flip happen on ONE gate snapshot, so a stop either fully lands BEFORE the commit (OCE, admission is
                // never opened, Begin never returns success) or wins AFTER it and is handled as a normal close:
                // stopRequested=1 makes AdmissionOpen false and every later group re-grants and refuses.
                if (shouldStop()) throw new OperationCanceledException("NC02_BEGIN_STOP_REQUESTED:group=NC01_BEGIN_GROUP_COMMIT|generation=" + generation.ToString(CultureInfo.InvariantCulture));
                bool beginCommitStopCancel;
                string beginCommitRejectReason;
                bool beginCommitted;
                lock (_gateLock)
                {
                    beginCommitted = RuntimeGroupGrant(generation, "NC01_BEGIN_GROUP_COMMIT", false, out beginCommitStopCancel, out beginCommitRejectReason);
                    if (beginCommitted)
                    {
                        Volatile.Write(ref _hostingState, 2);
                        Volatile.Write(ref _admissionOpen, 1);
                    }
                }
                if (!beginCommitted)
                {
                    if (beginCommitStopCancel) throw new OperationCanceledException("NC02_BEGIN_STOP_REQUESTED:group=NC01_BEGIN_GROUP_COMMIT|generation=" + generation.ToString(CultureInfo.InvariantCulture) + "|recheck=1");
                    throw new InvalidOperationException(beginCommitRejectReason);
                }
            }
            catch (OperationCanceledException)
            {
                // Normal stop: never written as a sticky FailureReason; the gate stays closed because RequestStop already
                // closed it (or will) — Begin never opened it on this path.
                lock (_gateLock) { Volatile.Write(ref _hostingState, 3); }
                throw;
            }
            catch
            {
                lock (_gateLock) { Volatile.Write(ref _hostingState, 3); }
                DisableCaptures("NC01_HOSTING_SETUP_FAILED");
                throw;
            }
        }

        private void BeginHostingLegacyCore()
        {
            if (Interlocked.CompareExchange(ref _hostingState, 1, 0) != 0) throw new InvalidOperationException("NC01_ALREADY_HOSTING");
            try
            {
                _baseline = MainSnapshotCore();
                _sessionId = Guid.NewGuid().ToString("N");
                Window mainWpf = Application.Current.MainWindow;
                if (mainWpf == null) throw new InvalidOperationException("NC01_NO_MAIN_WPF_WINDOW");
                PresentationSource src = PresentationSource.FromVisual(mainWpf);
                if (src == null || src.CompositionTarget == null) throw new InvalidOperationException("NC01_NO_COMPOSITION_TARGET");
                System.Windows.Media.Matrix device = src.CompositionTarget.TransformToDevice;
                if (!IsFinitePositive(device.M11) || !IsFinitePositive(device.M22)) throw new InvalidOperationException("NC01_DPI_SCALE_INVALID");
                _scaleX = device.M11; _scaleY = device.M22;
                for (int eye = 0; eye < 2; eye++)
                {
                    string eyeName = eye == 0 ? "LEFT" : "RIGHT";
                    IWindow3D window = _app.Create3DWindow(_app.World);
                    IViewport vm = _mainVp.CreateViewport(window);
                    vm.DisplayName = "NC01_STEREO_" + eyeName;
                    vm.IsSimulationControlVisible = false; vm.IsToolBarVisible = false; vm.IsCubeVisible = false;
                    // Field setters are no-ops until the SDK wires the eye's own traverser; copy is deferred to TryEyeReady behind target-RenderStages evidence.
                    // Direct3DViewportView is only populated after OnViewLoaded; reading it here would fail.
                    _eyes[eye] = window; _vms[eye] = vm;
                    _dock.ShowFloatingWindow("NC01_H2_" + eyeName + "_" + _sessionId, vm, new Size(TargetWidth / _scaleX + 40.0, TargetHeight / _scaleY + 60.0), null, false, false, true);
                }
                _hostingState = 2;
            }
            catch
            {
                _hostingState = 3;
                DisableCaptures("NC01_HOSTING_SETUP_FAILED");
                throw;
            }
        }

        // Pure managed stop surface for RequestStop/TryCompleteStop: no SDK getter, no external callback, no Dispose, and
        // the static lease/_runtimeOwnerStrongRef, the official Window/View/D3DImage references and the loaded helper all
        // survive for the process lifetime — nothing is unhooked, disposed, re-bootstrapped or written to renderService.
        public void RequestStop(int generation)
        {
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            // Bind-once claim, foreign-generation refusal and the permanent close happen on ONE gate snapshot under the
            // same short lock: no concurrent Begin/Ready/raw admission can interleave between refusal and close, and an
            // observer (StopVerified/TryCompleteStop) never sees a half-applied close or a stop-completion in the gap.
            // A stop arriving BEFORE the first Begin binds the generation and permanently closes this lease: the later
            // same-generation Begin is then refused by the stop check, and a foreign Begin cannot revive or re-bind.
            lock (_gateLock)
            {
                // May arrive BEFORE the first Begin: a bind-once CAS claims the generation; once bound it never changes.
                Interlocked.CompareExchange(ref _boundGeneration, generation, 0);
                int bound = Volatile.Read(ref _boundGeneration);
                // A foreign generation is refused without touching this lease; an idempotent same-generation stop re-closes
                // without reopening admission and without any SDK effect.
                if (bound != generation) throw new InvalidOperationException("NC01_STOP_FOREIGN_GENERATION:bound=" + FormatInvariant(bound) + "|requested=" + generation.ToString(CultureInfo.InvariantCulture));
                // Permanent close: stopRequested is the authoritative in-flight-group signal, admissionOpen is cleared with it.
                // A running group completes its already-entered required SDK calls and Unlock; later groups and later owner
                // calls are refused. No absolute millisecond stop deadline is promised: an entered GetRT/SurfaceRender group
                // may block until the SDK returns.
                Volatile.Write(ref _stopRequested, 1);
                Volatile.Write(ref _admissionOpen, 0);
            }
        }

        // Honest completion probe: true only for the same positively bound generation after its gate is permanently closed
        // AND every entered Begin/Ready/raw-pair/capture logical SDK transaction (including the D3DImage Unlock windows) has
        // exited. False while busy or for a foreign/unbound generation. Pure managed: no SDK call, callback or Dispose.
        public bool TryCompleteStop(int generation)
        {
            if (generation <= 0) return false;
            // One consistent managed snapshot under the gate lock: bound generation, permanent close and BOTH busy
            // registrations are judged together, so completion can never be reported in a gap between separate atomics
            // while a registered Begin/Ready/raw-pair call is still heading for its SDK reads.
            lock (_gateLock)
            {
                return Volatile.Read(ref _boundGeneration) == generation
                    && Volatile.Read(ref _stopRequested) != 0
                    && Volatile.Read(ref _runtimeCallsBusy) == 0
                    && Volatile.Read(ref _captureBusy) == 0
                    && _stagingHelperDrained
                    && Volatile.Read(ref _stagingPendingCount) == 0;
            }
        }

        // Pure managed group-entry gate shared by the runtime multi-group transactions. The external shouldStop callback
        // runs OUTSIDE the gate lock FIRST and may re-enter RequestStop while still returning false; the locked grant
        // below then re-verifies on ONE snapshot and must still refuse. The grant is the group's admission boundary: a
        // stop landing after the grant lets the already-granted group finish its required SDK calls (including the
        // mandatory D3DImage Unlock), keeps StopVerified false through the whole-call busy registration, and is honored
        // at the NEXT group, which must re-acquire admission. No SDK, SurfaceRender/GetRT/Unlock, callback or Dispose
        // work ever runs while the gate lock is held.
        private void RequireRuntimeGroup(int generation, Func<bool> shouldStop, string context, bool requireAdmissionOpen, string stopMessage)
        {
            if (shouldStop()) throw new OperationCanceledException(stopMessage + ":group=" + context + "|generation=" + generation.ToString(CultureInfo.InvariantCulture));
            bool stopCancel;
            string rejectReason;
            if (!RuntimeGroupGrant(generation, context, requireAdmissionOpen, out stopCancel, out rejectReason))
            {
                if (stopCancel) throw new OperationCanceledException(stopMessage + ":group=" + context + "|generation=" + generation.ToString(CultureInfo.InvariantCulture) + "|recheck=1");
                throw new InvalidOperationException(rejectReason);
            }
        }

        // The locked grant shared by EVERY runtime SDK group permission (Begin groups, Begin commit, Ready groups and
        // raw groups, including Ready which has no external callback): the original bound generation, the permanent
        // stop, the fault close, the required admission and the calling transaction's busy registration are re-verified
        // on ONE snapshot under the same short 03d1 _gateLock — no second lifecycle lock. stopCancel distinguishes a
        // Stop/same-lease-close cancel (Begin/raw propagate it as OperationCanceledException, Ready returns it as a
        // plain false with the cancel/closed reason) from foreign-generation or fault-closed rejects, which stay plain
        // refusals; a lost busy registration is an internal invariant refusal. Re-entrant: callers already holding
        // _gateLock may fold grant+managed-commit into one atomic transaction (Monitor re-entry).
        private bool RuntimeGroupGrant(int generation, string context, bool requireAdmissionOpen, out bool stopCancel, out string rejectReason)
        {
            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) { stopCancel = false; rejectReason = context + "_GENERATION_CHANGED:bound=" + FormatInvariant(bound) + "|transaction=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                if (Volatile.Read(ref _stopRequested) != 0) { stopCancel = true; rejectReason = context + "_STOP_REQUESTED:generation=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                if (_captureDisabled || Volatile.Read(ref _hostingState) == 3) { stopCancel = false; rejectReason = context + "_FAULT_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                if (requireAdmissionOpen && Volatile.Read(ref _admissionOpen) == 0) { stopCancel = false; rejectReason = context + "_ADMISSION_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                if (Volatile.Read(ref _runtimeCallsBusy) == 0) { stopCancel = false; rejectReason = context + "_BUSY_REGISTRATION_LOST:generation=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                stopCancel = false;
                rejectReason = null;
                return true;
            }
        }

        // Managed-only callback for runtime legacy overloads that must forward to the bound generation without adding a
        // new stop source of their own.
        private static readonly Func<bool> RuntimeNeverStop = delegate { return false; };

        public bool TryGetReady(out string reason)
        {
            VerifyAccess();
            reason = null;
            // Runtime legacy overload: forward to the bound generation or refuse while unbound — never an escape hatch
            // around the generation gate. Non-runtime behavior is unchanged.
            if (_runtimeBackend)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound <= 0) { reason = "NC01_RUNTIME_GENERATION_NOT_BOUND:readyLegacy"; return false; }
                return TryGetReady(bound, out reason);
            }
            if (_hostingState != 2) { reason = _hostingState == 3 ? _disabledReason : "NC01_NOT_HOSTED"; return false; }
            if (_captureDisabled) { reason = _disabledReason; return false; }
            try
            {
                if (!MainSnapshotCore().StrictEquals(_baseline)) { DisableCaptures("NC01_BASELINE_DRIFT"); reason = _disabledReason; return false; }
                for (int eye = 0; eye < 2; eye++)
                {
                    string eyeReason;
                    if (!TryEyeReady(eye, out eyeReason)) { reason = eyeReason; return false; }
                    if (!TryConfirmEyeStageBaseline(eye, out eyeReason)) { reason = eyeReason; return false; }
                }
                return true;
            }
            catch (Exception ex)
            {
                InvalidOperationException precise = ex as InvalidOperationException;
                DisableCaptures(precise != null && precise.Message != null && precise.Message.StartsWith("NC01_", StringComparison.Ordinal)
                    ? precise.Message
                    : "NC01_READY_CHECK_FAILED:actualtype=" + ex.GetType().FullName + "|actualmessage=" + ex.Message);
                reason = _disabledReason;
                return false;
            }
        }

        // Runtime generation Ready. Entry refuses foreign/unbound generations and a closed gate outright (late Ready can
        // only reject — it never reopens admission and never touches the quality generation). The identity bindings read
        // is the first group; per eye (TryEyeReady + stage confirmation), quality sampling/freeze and the final true-claim
        // are each one group; a stop landing before a group refuses that group with a plain false/cancel-closed reason and
        // performs NO sticky DisableCaptures write.
        public bool TryGetReady(int generation, out string reason)
        {
            VerifyAccess();
            reason = null;
            if (generation <= 0) { reason = "NC01_READY_GENERATION_INVALID:generation=" + FormatInvariant(generation); return false; }
            if (!_runtimeBackend) { reason = "NC01_READY_GENERATION_RUNTIME_ONLY:runtimeBackend=0"; return false; }
            // Generation match, permanent stop, admission, sticky fault and the logical busy registration are decided on
            // ONE snapshot under the gate lock: a RequestStop either fully lands BEFORE this decision (plain refusal, no
            // SDK read happens) or observes busy=1 and cannot report completion before the identity/quality SDK reads.
            // UI re-entry is refused by the busy flag without any sticky FailureReason write.
            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) { reason = "NC01_READY_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                if (Volatile.Read(ref _stopRequested) != 0 || Volatile.Read(ref _admissionOpen) == 0) { reason = "NC01_READY_ADMISSION_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                if (_captureDisabled || Volatile.Read(ref _hostingState) == 3) { reason = _disabledReason != null ? _disabledReason : "NC01_READY_FAULT_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                if (_runtimeCallsBusy != 0) { reason = "NC01_RUNTIME_CALL_BUSY:ready|generation=" + generation.ToString(CultureInfo.InvariantCulture); return false; }
                _runtimeCallsBusy = 1;
            }
            // The identity/quality SDK reads and the group checks run OUTSIDE the gate lock with busy already registered;
            // busy is returned under the same lock in all exit paths (success, refusal, pending, exception).
            try { return TryGetReadyRuntimeCore(generation, out reason); }
            finally { lock (_gateLock) { Volatile.Write(ref _runtimeCallsBusy, 0); } }
        }

        private bool TryGetReadyRuntimeCore(int generation, out string reason)
        {
            reason = null;
            // Runtime Ready no longer requires whole-session baseline equality (a desktop user may legitimately move the
            // main camera or change focus); it verifies construction-bound identity instead. The identity binding reads
            // run in the FIRST granted group; each eye-ready+stage sample, the quality sampling/freeze and the final
            // true-claim each pass the SAME locked grant (Ready has no external callback). Stop/same-lease-close
            // refusals return a plain false with the cancel/closed reason — no throw, no sticky DisableCaptures write,
            // and the existing user error is never cleared.
            if (_hostingState != 2) { reason = _hostingState == 3 ? _disabledReason : "NC01_NOT_HOSTED"; return false; }
            if (_captureDisabled) { reason = _disabledReason; return false; }
            try
            {
                bool stopCancel;
                string rejectReason;
                if (!RuntimeGroupGrant(generation, "NC01_READY_GROUP_IDENTITY", true, out stopCancel, out rejectReason))
                { reason = rejectReason; return false; }
                VerifyRuntimeIdentityBindings();
                for (int eye = 0; eye < 2; eye++)
                {
                    if (!RuntimeGroupGrant(generation, "NC01_READY_GROUP_EYE" + eye.ToString(CultureInfo.InvariantCulture), true, out stopCancel, out rejectReason))
                    { reason = rejectReason; return false; }
                    string eyeReason;
                    if (!TryEyeReady(eye, out eyeReason)) { reason = eyeReason; return false; }
                    if (!TryConfirmEyeStageBaseline(eye, out eyeReason)) { reason = eyeReason; return false; }
                }
                string qualityReason;
                if (!TryRuntimeQualityReady(generation, out qualityReason)) { reason = qualityReason; return false; }
                // Returning true is itself gated: one final locked grant re-verification before the ready claim leaves.
                if (!RuntimeGroupGrant(generation, "NC01_READY_GROUP_FINAL", true, out stopCancel, out rejectReason))
                { reason = rejectReason; return false; }
                return true;
            }
            catch (OperationCanceledException ex)
            {
                // A cancel never routes through the generic sticky-fault handler and never writes a FailureReason:
                // plain false with the cancel/closed reason.
                reason = "NC01_READY_CANCELLED:" + ex.Message;
                return false;
            }
            catch (Exception ex)
            {
                InvalidOperationException precise = ex as InvalidOperationException;
                DisableCaptures(precise != null && precise.Message != null && precise.Message.StartsWith("NC01_", StringComparison.Ordinal)
                    ? precise.Message
                    : "NC01_READY_CHECK_FAILED:actualtype=" + ex.GetType().FullName + "|actualmessage=" + ex.Message);
                reason = _disabledReason;
                return false;
            }
        }

        private bool TryConfirmEyeStageBaseline(int eye, out string reason)
        {
            string current = ReadRenderStagesOrNullWhenEmpty(_eyes[eye]);
            if (current == null) { reason = "NC01_EYE_STAGES_NOT_READY"; return false; }
            if (_eyeStageSnapshots[eye] != null)
            {
                if (!string.Equals(_eyeStageSnapshots[eye], current, StringComparison.Ordinal))
                { DisableCaptures("NC01_EYE_STAGE_DRIFT"); reason = _disabledReason; return false; }
                reason = null; return true;
            }
            if (_eyeStagePending[eye] != null && string.Equals(_eyeStagePending[eye], current, StringComparison.Ordinal))
            { _eyeStageSnapshots[eye] = current; _eyeStagePending[eye] = null; reason = null; return true; }
            _eyeStagePending[eye] = current;
            reason = "NC01_EYE_STAGES_BASELINE_PENDING";
            return false;
        }

        // Runtime identity gate: read-only verification, per Ready and per raw pair, that the actual objects bound at
        // construction are still the same references with the same main dimensions — never by active-focus hash.
        // Any drift sticky-closes this lease; nothing is written to main and nothing is rebuilt.
        private void VerifyRuntimeIdentityBindings()
        {
            if (!ReferenceEquals(_mainVp.Window, _mainWindow))
                RejectRuntimeIdentityDrift("NC01_RUNTIME_MAIN_VIEWPORT_WINDOW_DRIFT");
            if (!ReferenceEquals(_app.World, _runtimeWorldStrongRef))
                RejectRuntimeIdentityDrift("NC01_RUNTIME_WORLD_REF_DRIFT");
            ICamera currentCamera = _mainWindow.Camera;
            if (!ReferenceEquals(currentCamera, _runtimeMainCameraStrongRef))
            {
                object currentPointer = ReflectTryGetter(currentCamera, "Pointer");
                RejectRuntimeIdentityDrift("NC01_RUNTIME_MAIN_CAMERA_REF_DRIFT:boundPointerHex=" + _runtimeMainCameraPointerEvidence
                    + "|currentPointerHex=" + (currentPointer is IntPtr ? ((IntPtr)currentPointer).ToInt64().ToString("x16", CultureInfo.InvariantCulture) : "<unreadable>")
                    + "|currentType=" + (currentCamera == null ? "<null>" : currentCamera.GetType().FullName));
            }
            double currentWidth = _mainWindow.Width, currentHeight = _mainWindow.Height;
            if (currentWidth != _runtimeMainWidth || currentHeight != _runtimeMainHeight)
                RejectRuntimeIdentityDrift("NC01_RUNTIME_MAIN_DIMS_DRIFT:boundWidth=" + FormatInvariant(_runtimeMainWidth)
                    + "|boundHeight=" + FormatInvariant(_runtimeMainHeight)
                    + "|currentWidth=" + FormatInvariant(currentWidth) + "|currentHeight=" + FormatInvariant(currentHeight));
        }

        private void RejectRuntimeIdentityDrift(string reason)
        {
            DisableCaptures(reason);
            throw new InvalidOperationException(reason);
        }

        // Freezes the managed quality fingerprint only after the caller has proven both eyes DXReady, owned-1024, display
        // copy completed and stage baselines stable (two consecutive identical readings required). QualityGeneration stays 0
        // while pending; once frozen at 1, any drift sticky-rejects — never a re-copy or re-bootstrap. The SDK sampling and
        // fingerprint build (MainSnapshotCore + own-eye display getters) run OUTSIDE the gate lock; every managed quality
        // mutation (pending write, freeze, drift close) is linearized with RequestStop by re-verifying the grant inside the
        // SAME commit lock, so a stop that won during the sampling can never be followed by a freeze or a new sticky drift.
        private bool TryRuntimeQualityReady(int generation, out string reason)
        {
            bool sampleStopCancel;
            string sampleRejectReason;
            if (!RuntimeGroupGrant(generation, "NC01_READY_GROUP_QUALITY", true, out sampleStopCancel, out sampleRejectReason))
            { reason = sampleRejectReason; return false; }
            string fingerprint, buildError;
            string[] currentSubset;
            if (!TryBuildRuntimeQualityFingerprint(MainSnapshotCore().Serialized, out fingerprint, out currentSubset, out buildError))
            { DisableCaptures(buildError); reason = _disabledReason; return false; }
            bool granted;
            bool stopCancel;
            string rejectReason;
            bool readyHit = false;
            bool pendingWait = false;
            lock (_gateLock)
            {
                granted = RuntimeGroupGrant(generation, "NC01_READY_GROUP_QUALITY_COMMIT", true, out stopCancel, out rejectReason);
                if (granted)
                {
                    if (_qualityGeneration == 1)
                    {
                        if (string.Equals(_runtimeQualityFingerprint, fingerprint, StringComparison.Ordinal)) readyHit = true;
                        else
                        {
                            DisableCaptures("NC01_RUNTIME_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(currentSubset));
                            readyHit = false;
                            pendingWait = false;
                        }
                    }
                    else if (_qualityPendingFingerprint != null && string.Equals(_qualityPendingFingerprint, fingerprint, StringComparison.Ordinal))
                    {
                        _runtimeQualityFingerprint = fingerprint;
                        _frozenQualitySubset = currentSubset;
                        _qualityPendingFingerprint = null;
                        Volatile.Write(ref _qualityGeneration, 1);
                        readyHit = true;
                    }
                    else
                    {
                        _qualityPendingFingerprint = fingerprint;
                        pendingWait = true;
                    }
                }
            }
            if (!granted) { reason = rejectReason; return false; }
            if (readyHit) { reason = null; return true; }
            if (pendingWait) { reason = "NC01_RUNTIME_QUALITY_PENDING"; return false; }
            reason = _disabledReason;
            return false;
        }

        private string DescribeRuntimeQualityDrift(string[] currentSubset)
        {
            if (_frozenQualitySubset == null || currentSubset == null
                || _frozenQualitySubset.Length != currentSubset.Length || RuntimeQualitySubsetNames.Length != currentSubset.Length)
                return ":detail=fingerprint-ordering-or-eye-stage-changed";
            for (int i = 0; i < currentSubset.Length; i++)
                if (!string.Equals(_frozenQualitySubset[i], currentSubset[i], StringComparison.Ordinal))
                    return ":field=" + RuntimeQualitySubsetNames[i]
                        + "|frozen=" + TruncateForDiagnostic(_frozenQualitySubset[i])
                        + "|current=" + TruncateForDiagnostic(currentSubset[i]);
            return ":detail=eye-stage-changed";
        }

        // Reuses the existing ordered snapshot parser; verifies both eyes carry copied display fields and each a non-empty
        // stable stage snapshot. The eye stage text participates in the fingerprint so own-stage drift changes it.
        private bool TryBuildRuntimeQualityFingerprint(string mainSnapshotSerialized, out string fingerprint, out string[] mainSubset, out string error)
        {
            fingerprint = null; mainSubset = null; error = null;
            string[] fields;
            try { fields = ParseMainSnapshotOrdered(mainSnapshotSerialized, "runtime-quality"); }
            catch (InvalidOperationException ex) { error = ex.Message; return false; }
            StringBuilder sb = new StringBuilder("NC01RQ1");
            mainSubset = new string[RuntimeQualitySubsetIndices.Length];
            for (int i = 0; i < RuntimeQualitySubsetIndices.Length; i++)
            {
                mainSubset[i] = fields[RuntimeQualitySubsetIndices[i]];
                sb.Append('|').Append(RuntimeQualitySubsetNames[i]).Append('=').Append(mainSubset[i]);
            }
            for (int eye = 0; eye < 2; eye++)
            {
                string eyeTag = eye.ToString(CultureInfo.InvariantCulture);
                if (!_eyeDisplayCopied[eye]) { error = "NC01_RUNTIME_QUALITY_EYE_DISPLAY_NOT_COPIED:eye=" + eyeTag; return false; }
                string stage = _eyeStageSnapshots[eye];
                if (string.IsNullOrEmpty(stage)) { error = "NC01_RUNTIME_QUALITY_EYE_STAGE_NOT_STABLE:eye=" + eyeTag; return false; }
                string[] eyeValues;
                try { eyeValues = ReadEyeDisplayFields(_eyes[eye]); }
                catch (Exception ex)
                { error = "NC01_RUNTIME_QUALITY_EYE_DISPLAY_READ_FAILED:eye=" + eyeTag + "|originaltype=" + ex.GetType().FullName + "|originalmessage=" + ex.Message; return false; }
                for (int k = 0; k < 8; k++)
                    if (!string.Equals(eyeValues[k], mainSubset[k], StringComparison.Ordinal))
                    {
                        error = "NC01_RUNTIME_QUALITY_EYE_DISPLAY_MISMATCH:eye=" + eyeTag + "|field=" + RuntimeQualitySubsetNames[k]
                            + "|main=" + TruncateForDiagnostic(mainSubset[k]) + "|eye=" + TruncateForDiagnostic(eyeValues[k]);
                        return false;
                    }
                sb.Append("|eye").Append(eyeTag).Append("Stage=").Append(stage);
            }
            fingerprint = sb.ToString();
            return true;
        }

        private static string[] ReadEyeDisplayFields(IWindow3D eyeWindow)
        {
            return new string[]
            {
                FormatInvariant(eyeWindow.UseHeadlight), FormatInvariant(eyeWindow.EdgeShadingMode),
                FormatInvariant(eyeWindow.EdgeColorMode), FormatInvariant(eyeWindow.RenderShadingMode),
                FormatInvariant(eyeWindow.RenderOpacity), FormatInvariant(eyeWindow.UseMaterialShaders),
                ColorRgba4(eyeWindow.BackgroundColor), FormatInvariant(eyeWindow.TransparencyQuality)
            };
        }

        // Runtime instances refuse this legacy entry UNCONDITIONALLY before any SDK getter: an open admission gate alone
        // cannot promise the generation contract (it registers no busy and cannot bind a stopped transaction), so the
        // former half-guard is removed. Every runtime SDK read happens inside an admitted generation transaction which
        // calls MainSnapshotCore directly. Non-runtime behavior is unchanged.
        public NC01MainSnapshotV1 MainSnapshot()
        {
            if (_runtimeBackend)
                throw new InvalidOperationException("NC01_RUNTIME_LEGACY_SDK_ENTRY_REJECTED:mainSnapshot|boundGeneration=" + FormatInvariant(Volatile.Read(ref _boundGeneration)));
            return MainSnapshotCore();
        }

        private NC01MainSnapshotV1 MainSnapshotCore()
        {
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
            return MainSnapshotCore(null, -1);
#endif
            VerifyAccess();
            StringBuilder sb = new StringBuilder();
            IWindow3D w = _mainWindow, active = _app.ActiveWindow;
            ICamera c = w.Camera;
            Append(sb, "activeWindowHash", RuntimeHelpers.GetHashCode(active)); Append(sb, "activeIsMain", ReferenceEquals(active, w));
            Append(sb, "mainWidth", w.Width); Append(sb, "mainHeight", w.Height);
            Append(sb, "cameraIdentity", RuntimeHelpers.GetHashCode(c));
            double[] m16 = ToMat16(c.ViewMatrix);
            for (int i = 0; i < 16; i++) Append(sb, "view" + i.ToString(CultureInfo.InvariantCulture), m16[i]);
            Append(sb, "FieldOfViewY", c.FieldOfViewY); Append(sb, "AspectRatio", c.AspectRatio);
            Append(sb, "GazeLength", c.GazeLength); Append(sb, "IsOrthographic", c.IsOrthographic);
            Append(sb, "UseHeadlight", w.UseHeadlight); Append(sb, "EdgeShadingMode", w.EdgeShadingMode);
            Append(sb, "EdgeColorMode", w.EdgeColorMode); Append(sb, "RenderShadingMode", w.RenderShadingMode);
            Append(sb, "RenderOpacity", w.RenderOpacity); Append(sb, "UseMaterialShaders", w.UseMaterialShaders);
            Append(sb, "BackgroundColor", ColorRgba4(w.BackgroundColor)); Append(sb, "TransparencyQuality", w.TransparencyQuality);
            Append(sb, "displayQuality", ReadConfiguredDisplayQuality());
            Append(sb, "actualRenderFlags", ReadActualRenderServiceFlags());
            Append(sb, "mainRenderStages", ReadRenderStages(w));
            return new NC01MainSnapshotV1(sb.ToString());
        }

#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OWNER_PHASE_CPU_WALL
        // MAIN_SNAPSHOT_INNER_CORE_BEGIN
        // Same shape as the legacy MainSnapshotCore (same Append order, same window/camera/flags/stages reads, same
        // returned string form, same exception paths) plus six NONTHROWING Stopwatch.GetTimestamp() readings
        // bracketing exactly the three inner helpers (displayQuality=Qc, actualRenderFlags=Fl, mainRenderStages=Rs).
        // When row is null (no inner sample requested) or phase is not an open P0/P4 boundary, the readings never
        // touch row state; a timestamp failure or a second-call-during-open-phase duplicate only marks that phase's
        // inner timing invalid and the helper result/exception path is unchanged. Only the three SharedGPU P0/P4
        // call sites pass (row, phase); every other MainSnapshot consumer keeps zero inner sample.
        // SINGLE NAMED POLICY CHANGE (triple-macro CONFIGURED-QUALITY ONLY): the qaTextQc assignment reuses the
        // readiness-frozen configured-quality string produced by the original two identical fresh readiness snapshots and
        // the original structural parser — a QA-only bound-session snapshot policy, NOT a live-configuration oracle.
        // Reuse happens only for an admitted valid inner P0/P4 snapshot with quality generation 1, a positive bound
        // generation, admission open and stop not requested; non-inner/unready/closing uses the original fresh read.
        // Because the string is frozen at readiness, quality edits not yet applied to the renderer, file
        // corruption/replacement and change-and-revert can be ignored until a future original fresh readiness boundary.
        // SIM Pause is not such a boundary: this owner has no Pause API, Stop is permanent and a new complete capture
        // session requires a new owner. actualRenderFlags, viewport/camera values and mainRenderStages still read fresh
        // on every call and are compared before/after every capture. The fresh helper's normal-return 12 CPU-wall
        // diagnostics count only true fresh helper calls; frozen bound-session reuse never touches those counts, and
        // that 12 count is not a total snapshot count and not the same 79/31 cohort carrier.
        private NC01MainSnapshotV1 MainSnapshotCore(NC01GpuSplitRowQa row, int phase)
        {
            bool inner = row != null && (phase == 0 || phase == 4) && ReferenceEquals(row.Owner, this);
            if (inner)
            {
                NC01OwnerPhaseCpuWallCacheQa gate = _qaOwnerPhaseCpuWallCacheQa;
                bool gateOk = gate != null && gate.SchemaVersion == 1 && gate.Enabled == 1 && gate.GateStatus == 1 && gate.Frequency > 0L;
                bool openPhaseOk = row.OpenPhaseId == phase && (row.OpenPhaseMask & (1u << phase)) != 0u && row.OpenStartQpc > 0L;
                int attempt = phase == 0 ? row.P0InnerAttempt : row.P4InnerAttempt;
                int open = phase == 0 ? row.P0InnerOpen : row.P4InnerOpen;
                if (gateOk && openPhaseOk && open == 1 && attempt == 0)
                {
                    // First legal entry for this phase with the frozen cold gate on: sample.
                }
                else
                {
                    if (attempt == 1 && open == 1)
                    {
                        // Second entry while the phase's open bracket is still latched: duplicate, so mark invalid and never read again.
                        if (phase == 0) { row.P0InnerAttempt = 1; row.P0InnerReturn = 0; row.P0InnerKnown = 0; row.P0InnerInvalid = 1; row.P0InnerOpen = 2; }
                        else { row.P4InnerAttempt = 1; row.P4InnerReturn = 0; row.P4InnerKnown = 0; row.P4InnerInvalid = 1; row.P4InnerOpen = 2; }
                    }
                    else
                    {
                        // Cold gate off, no matching open phase identity, or no open inner bracket: missing/unreturned, mark invalid.
                        if (phase == 0) { row.P0InnerAttempt = 1; row.P0InnerReturn = 0; row.P0InnerKnown = 0; row.P0InnerInvalid = 1; }
                        else { row.P4InnerAttempt = 1; row.P4InnerReturn = 0; row.P4InnerKnown = 0; row.P4InnerInvalid = 1; }
                    }
                    inner = false;
                }
            }

            VerifyAccess();
            StringBuilder sb = new StringBuilder();
            IWindow3D w = _mainWindow, active = _app.ActiveWindow;
            ICamera c = w.Camera;
            Append(sb, "activeWindowHash", RuntimeHelpers.GetHashCode(active)); Append(sb, "activeIsMain", ReferenceEquals(active, w));
            Append(sb, "mainWidth", w.Width); Append(sb, "mainHeight", w.Height);
            Append(sb, "cameraIdentity", RuntimeHelpers.GetHashCode(c));
            double[] m16 = ToMat16(c.ViewMatrix);
            for (int i = 0; i < 16; i++) Append(sb, "view" + i.ToString(CultureInfo.InvariantCulture), m16[i]);
            Append(sb, "FieldOfViewY", c.FieldOfViewY); Append(sb, "AspectRatio", c.AspectRatio);
            Append(sb, "GazeLength", c.GazeLength); Append(sb, "IsOrthographic", c.IsOrthographic);
            Append(sb, "UseHeadlight", w.UseHeadlight); Append(sb, "EdgeShadingMode", w.EdgeShadingMode);
            Append(sb, "EdgeColorMode", w.EdgeColorMode); Append(sb, "RenderShadingMode", w.RenderShadingMode);
            Append(sb, "RenderOpacity", w.RenderOpacity); Append(sb, "UseMaterialShaders", w.UseMaterialShaders);
            Append(sb, "BackgroundColor", ColorRgba4(w.BackgroundColor)); Append(sb, "TransparencyQuality", w.TransparencyQuality);

            long qcStart = 0L, qcEnd = 0L, flStart = 0L, flEnd = 0L, rsStart = 0L, rsEnd = 0L;

            if (inner) { qcStart = QaInnerTimestampOrZero(); }
            string qaTextQc = MainSnapshotConfiguredQualityBoundSnapshotQa(inner);
            if (inner) { qcEnd = QaInnerTimestampOrZero(); }
            Append(sb, "displayQuality", qaTextQc);

            if (inner) { flStart = QaInnerTimestampOrZero(); }
            string qaTextFl = ReadActualRenderServiceFlags();
            if (inner) { flEnd = QaInnerTimestampOrZero(); }
            Append(sb, "actualRenderFlags", qaTextFl);

            if (inner) { rsStart = QaInnerTimestampOrZero(); }
            string qaTextRs = ReadRenderStages(w);
            if (inner) { rsEnd = QaInnerTimestampOrZero(); }
            Append(sb, "mainRenderStages", qaTextRs);

            if (inner)
            {
                if (qcStart <= 0L || qcEnd < qcStart || flStart <= 0L || flEnd < flStart || rsStart <= 0L || rsEnd < rsStart
                    || qcEnd > flStart || flEnd > rsStart)
                {
                    // Monotonicity / phase-boundary violated: this sample is invalid but the snapshot itself is well-formed.
                    if (phase == 0) { row.P0InnerAttempt = 1; row.P0InnerReturn = 1; row.P0InnerKnown = 0; row.P0InnerInvalid = 1; }
                    else { row.P4InnerAttempt = 1; row.P4InnerReturn = 1; row.P4InnerKnown = 0; row.P4InnerInvalid = 1; }
                }
                else if (phase == 0)
                {
                    row.P0InnerQcStart = qcStart; row.P0InnerQcEnd = qcEnd;
                    row.P0InnerFlStart = flStart; row.P0InnerFlEnd = flEnd;
                    row.P0InnerRsStart = rsStart; row.P0InnerRsEnd = rsEnd;
                    row.P0InnerAttempt = 1; row.P0InnerReturn = 1; row.P0InnerKnown = 1; row.P0InnerInvalid = 0;
                }
                else
                {
                    row.P4InnerQcStart = qcStart; row.P4InnerQcEnd = qcEnd;
                    row.P4InnerFlStart = flStart; row.P4InnerFlEnd = flEnd;
                    row.P4InnerRsStart = rsStart; row.P4InnerRsEnd = rsEnd;
                    row.P4InnerAttempt = 1; row.P4InnerReturn = 1; row.P4InnerKnown = 1; row.P4InnerInvalid = 0;
                }
            }
            return new NC01MainSnapshotV1(sb.ToString());
        }

        // Nonthrowing primitive read: only GetTimestamp is guarded; failure returns 0 so the sample flags itself invalid.
        private static long QaInnerTimestampOrZero()
        {
            try { return Stopwatch.GetTimestamp(); }
            catch (Exception) { return 0L; }
        }
        // MAIN_SNAPSHOT_INNER_CORE_END
#endif


        // Order MUST stay identical to MainSnapshot()'s Append sequence; the parser below depends on it.
        private static readonly string[] SnapshotFieldOrder = {
            "activeWindowHash", "activeIsMain", "mainWidth", "mainHeight", "cameraIdentity",
            "view0", "view1", "view2", "view3", "view4", "view5", "view6", "view7",
            "view8", "view9", "view10", "view11", "view12", "view13", "view14", "view15",
            "FieldOfViewY", "AspectRatio", "GazeLength", "IsOrthographic",
            "UseHeadlight", "EdgeShadingMode", "EdgeColorMode", "RenderShadingMode",
            "RenderOpacity", "UseMaterialShaders", "BackgroundColor", "TransparencyQuality",
            "displayQuality", "actualRenderFlags", "mainRenderStages" };

        // The only 20 top-level keys legitimately changed by a desktop camera between runs; everything else (including
        // cameraIdentity/activeWindowHash/activeIsMain/mainWidth/mainHeight/display/quality/render/stages) stays STRICT across runs.
        private static readonly bool[] CrossRunCameraFieldMask = BuildCrossRunCameraFieldMask();

        private static bool[] BuildCrossRunCameraFieldMask()
        {
            string[] allowed = {
                "view0", "view1", "view2", "view3", "view4", "view5", "view6", "view7", "view8", "view9",
                "view10", "view11", "view12", "view13", "view14", "view15",
                "FieldOfViewY", "AspectRatio", "GazeLength", "IsOrthographic" };
            bool[] mask = new bool[SnapshotFieldOrder.Length];
            int marked = 0;
            for (int a = 0; a < allowed.Length; a++)
            {
                int index = Array.IndexOf(SnapshotFieldOrder, allowed[a]);
                if (index >= 0 && !mask[index]) { mask[index] = true; marked++; }
            }
            if (marked != 20) throw new InvalidOperationException("NC01_SNAPSHOT_CAMERA_ALLOWLIST_BUILD_FAILED:marked=" + marked.ToString(CultureInfo.InvariantCulture));
            return mask;
        }

        // Runtime managed quality fingerprint subset: exactly the main eight display fields plus displayQuality,
        // actualRenderFlags and mainRenderStages. activeWindowHash/activeIsMain/cameraIdentity/mainWidth/mainHeight and the
        // 20 camera-state fields are excluded here; identity and dimensions are gated independently by the runtime bindings check.
        private static readonly string[] RuntimeQualitySubsetNames = {
            "UseHeadlight", "EdgeShadingMode", "EdgeColorMode", "RenderShadingMode", "RenderOpacity",
            "UseMaterialShaders", "BackgroundColor", "TransparencyQuality",
            "displayQuality", "actualRenderFlags", "mainRenderStages" };
        private static readonly int[] RuntimeQualitySubsetIndices = BuildRuntimeQualitySubsetIndices();

        private static int[] BuildRuntimeQualitySubsetIndices()
        {
            int[] indices = new int[RuntimeQualitySubsetNames.Length];
            for (int i = 0; i < RuntimeQualitySubsetNames.Length; i++)
            {
                int index = Array.IndexOf(SnapshotFieldOrder, RuntimeQualitySubsetNames[i]);
                if (index < 0) throw new InvalidOperationException("NC01_RUNTIME_QUALITY_SUBSET_FIELD_MISSING:" + RuntimeQualitySubsetNames[i]);
                indices[i] = index;
            }
            return indices;
        }

        // Explicit ordered 36-field parser mirroring MainSnapshot()'s Append grammar. Nested semicolons inside
        // displayQuality/actualRenderFlags/mainRenderStages are consumed structurally; missing, out-of-order, duplicate,
        // unknown or trailing fields reject. Never a naive Split and never a broad regex.
        private static string[] ParseMainSnapshotOrdered(string s, string context)
        {
            if (string.IsNullOrEmpty(s)) throw SnapshotParseFailed(context, "empty", 0, "");
            string[] values = new string[SnapshotFieldOrder.Length];
            int pos = 0;
            for (int i = 0; i <= 32; i++) values[i] = ReadSnapshotScalarField(s, ref pos, SnapshotFieldOrder[i], context);
            values[33] = ReadSnapshotGroupField(s, ref pos, SnapshotFieldOrder[33], QualityFieldNames, context);
            values[34] = ReadSnapshotGroupField(s, ref pos, SnapshotFieldOrder[34], ActualRenderFlagNames, context);
            values[35] = ReadSnapshotStageField(s, ref pos, context);
            if (pos != s.Length) throw SnapshotParseFailed(context, "trailing", pos, s.Substring(pos));
            return values;
        }

        private static string ReadSnapshotScalarField(string s, ref int pos, string name, string context)
        {
            string prefix = name + "=";
            if (!MatchesPrefix(s, pos, prefix)) throw SnapshotParseFailed(context, "field-missing-or-out-of-order", pos, prefix);
            pos += prefix.Length;
            int semi = s.IndexOf(';', pos);
            if (semi < 0) throw SnapshotParseFailed(context, "terminator-missing", pos, name);
            string value = s.Substring(pos, semi - pos);
            if (value.Length == 0 || value.IndexOf('=') >= 0) throw SnapshotParseFailed(context, "empty-or-embedded-delimiter", pos, name);
            pos = semi + 1;
            return value;
        }

        private static string ReadSnapshotGroupField(string s, ref int pos, string name, string[] subNames, string context)
        {
            string prefix = name + "=";
            if (!MatchesPrefix(s, pos, prefix)) throw SnapshotParseFailed(context, "field-missing-or-out-of-order", pos, prefix);
            pos += prefix.Length;
            int innerStart = pos;
            for (int i = 0; i < subNames.Length; i++)
            {
                string subPrefix = subNames[i] + "=";
                if (!MatchesPrefix(s, pos, subPrefix)) throw SnapshotParseFailed(context, "sub-field-missing-or-out-of-order", pos, name + "." + subNames[i]);
                pos += subPrefix.Length;
                int semi = s.IndexOf(';', pos);
                if (semi < 0) throw SnapshotParseFailed(context, "sub-terminator-missing", pos, name + "." + subNames[i]);
                string subValue = s.Substring(pos, semi - pos);
                if (subValue.Length == 0 || subValue.IndexOf('=') >= 0) throw SnapshotParseFailed(context, "sub-empty-or-embedded-delimiter", pos, name + "." + subNames[i]);
                pos = semi + 1;
            }
            int innerEnd = pos;
            if (pos >= s.Length || s[pos] != ';') throw SnapshotParseFailed(context, "field-terminator-missing", pos, name);
            pos++;
            return s.Substring(innerStart, innerEnd - innerStart);
        }

        private static string ReadSnapshotStageField(string s, ref int pos, string context)
        {
            string prefix = SnapshotFieldOrder[35] + "=";
            if (!MatchesPrefix(s, pos, prefix)) throw SnapshotParseFailed(context, "field-missing-or-out-of-order", pos, prefix);
            pos += prefix.Length;
            int innerStart = pos;
            int ordinal = 0;
            while (true)
            {
                string header = "stage" + ordinal.ToString(CultureInfo.InvariantCulture) + "|pointerHex=";
                if (!MatchesPrefix(s, pos, header)) break;
                pos += header.Length;
                int bar = s.IndexOf('|', pos);
                if (bar < 0) throw SnapshotParseFailed(context, "stage-delimiter-missing", pos, header);
                string pointerHex = s.Substring(pos, bar - pos);
                if (!IsLowerHex16(pointerHex)) throw SnapshotParseFailed(context, "stage-pointer-hex-invalid", pos, pointerHex);
                pos = bar + 1;
                if (!MatchesPrefix(s, pos, "nameBase64=")) throw SnapshotParseFailed(context, "stage-namebase64-missing", pos, "stage" + ordinal.ToString(CultureInfo.InvariantCulture));
                pos += "nameBase64=".Length;
                bar = s.IndexOf('|', pos);
                if (bar < 0) throw SnapshotParseFailed(context, "stage-delimiter-missing", pos, "nameBase64");
                string nameBase64 = s.Substring(pos, bar - pos);
                if (!IsBase64Chars(nameBase64)) throw SnapshotParseFailed(context, "stage-namebase64-invalid", pos, nameBase64);
                pos = bar + 1;
                if (!MatchesPrefix(s, pos, "enabled=")) throw SnapshotParseFailed(context, "stage-enabled-missing", pos, "stage" + ordinal.ToString(CultureInfo.InvariantCulture));
                pos += "enabled=".Length;
                if (pos >= s.Length || (s[pos] != '0' && s[pos] != '1')) throw SnapshotParseFailed(context, "stage-enabled-invalid", pos, "stage" + ordinal.ToString(CultureInfo.InvariantCulture));
                pos++;
                if (pos >= s.Length || s[pos] != ';') throw SnapshotParseFailed(context, "stage-terminator-missing", pos, "stage" + ordinal.ToString(CultureInfo.InvariantCulture));
                pos++;
                ordinal++;
            }
            if (ordinal == 0) throw SnapshotParseFailed(context, "stage-sequence-empty", pos, SnapshotFieldOrder[35]);
            int innerEnd = pos;
            if (pos >= s.Length || s[pos] != ';') throw SnapshotParseFailed(context, "field-terminator-missing", pos, SnapshotFieldOrder[35]);
            pos++;
            return s.Substring(innerStart, innerEnd - innerStart);
        }

        private static bool MatchesPrefix(string s, int pos, string prefix)
        {
            return pos >= 0 && prefix.Length <= s.Length - pos && string.CompareOrdinal(s, pos, prefix, 0, prefix.Length) == 0;
        }

        private static bool IsLowerHex16(string value)
        {
            if (value == null || value.Length != 16) return false;
            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            }
            return true;
        }

        // An empty stage name is legal in the installed SDK (nameBase64=); names are not identities (Pointer+ordinal is),
        // so empty and duplicate names must parse. Non-empty values must round-trip as canonical base64.
        private static bool IsBase64Chars(string value)
        {
            if (value == null) return false;
            if (value.Length == 0) return true;
            try
            {
                byte[] decoded = Convert.FromBase64String(value);
                return string.Equals(Convert.ToBase64String(decoded), value, StringComparison.Ordinal);
            }
            catch (FormatException) { return false; }
        }

        private static InvalidOperationException SnapshotParseFailed(string context, string problem, int offset, string detail)
        {
            return new InvalidOperationException("NC01_SNAPSHOT_PARSE_FAILED:" + context + "|problem=" + problem
                + "|offset=" + offset.ToString(CultureInfo.InvariantCulture)
                + "|detail=" + TruncateForDiagnostic(detail));
        }

        private static string TruncateForDiagnostic(string value)
        {
            if (value == null) return "<null>";
            if (value.Length <= 128) return value;
            return value.Substring(0, 128) + "...(len=" + value.Length.ToString(CultureInfo.InvariantCulture) + ")";
        }

        public NC01CaptureSampleV1 CaptureEye(int eye, Matrix worldFromCamera)
        {
            // Runtime instances refuse this legacy world-matrix entry outright BEFORE any SDK getter/setter or busy
            // registration: it cannot honor the generation contract. Runtime captures happen only inside an admitted
            // CaptureRawPair(generation) transaction. The full non-runtime body and capture timing are unchanged.
            if (_runtimeBackend) throw new InvalidOperationException("NC01_RUNTIME_LEGACY_SDK_ENTRY_REJECTED:captureEye|boundGeneration=" + FormatInvariant(Volatile.Read(ref _boundGeneration)));
            VerifyAccess();
            if (eye != 0 && eye != 1) throw new ArgumentOutOfRangeException("eye");
            if (_hostingState != 2) throw new InvalidOperationException("NC01_NOT_HOSTED");
            if (_captureDisabled) throw new InvalidOperationException("NC01_CAPTURE_DISABLED:" + _disabledReason);
            if (Interlocked.CompareExchange(ref _captureBusy, 1, 0) != 0) throw new InvalidOperationException("NC01_CAPTURE_BUSY");
            try { return CaptureEyeCore(eye, worldFromCamera); }
            catch
            {
                if (!_captureDisabled) DisableCaptures("NC01_CAPTURE_FAULT");
                try { _lastAfter = MainSnapshotCore(); } catch (Exception) { }
                throw;
            }
            finally { Volatile.Write(ref _captureBusy, 0); }
        }

        private NC01CaptureSampleV1 CaptureEyeCore(int eye, Matrix worldFromCamera)
        {
            string readyReason;
            if (!TryGetReady(out readyReason)) throw new InvalidOperationException("NC01_CAPTURE_REFUSED:" + readyReason);
            EnsureEyeStagesMatchBaseline(eye);
            _lastBefore = MainSnapshotCore();
            IntPtr sceneBefore = RequireScenePointer(eye);
            ICamera cam = _eyes[eye].Camera;
            Matrix wantView = worldFromCamera.Inverse();
            cam.ViewMatrix = wantView;
            double[] actual16 = ToMat16(cam.ViewMatrix);
            if (!MatricesClose(ToMat16(wantView), actual16)) throw new InvalidOperationException("NC01_CAMERA_WRITEBACK_MISMATCH");
            double fovY = cam.FieldOfViewY, aspect = cam.AspectRatio, gaze = cam.GazeLength;
            bool ortho = cam.IsOrthographic;
            if (!_images[eye].TryLock(new Duration(TimeSpan.FromMilliseconds(10)))) throw new InvalidOperationException("NC01_DX_LOCK_TIMEOUT");
            Stopwatch sw = Stopwatch.StartNew();
            byte[] bgra;
            NC01SurfaceInfoV1 info = default(NC01SurfaceInfoV1);
            int rc;
            try
            {
                int renderRc = _app.SurfaceRender(_eyes[eye]);
                sw.Stop();
                // Official SDK treats a negative SurfaceRender return as failure; never read back a stale surface.
                if (renderRc < 0) throw new InvalidOperationException("NC01_SURFACE_RENDER_FAILED:actualrc=" + renderRc.ToString(CultureInfo.InvariantCulture));
                if (RequireScenePointer(eye) != sceneBefore) throw new InvalidOperationException("NC01_SCENE_IDENTITY_CHANGED");
                EnsureEyeStagesMatchBaseline(eye);
                bgra = new byte[TargetWidth * TargetHeight * 4];
                rc = NC01_ReadSurface(sceneBefore, (uint)TargetWidth, (uint)TargetHeight, bgra,
                    (ulong)bgra.LongLength, ref info, (uint)Marshal.SizeOf(typeof(NC01SurfaceInfoV1)));
            }
            finally { _images[eye].Unlock(); }
            _lastAfter = MainSnapshotCore();
            if (rc != 0) throw new InvalidOperationException("NC01_READBACK_FAILED:" + rc.ToString(CultureInfo.InvariantCulture));
            if (info.StructBytes != 64u || info.Version != 1u) throw new InvalidOperationException("NC01_ABI_MISMATCH");
            if (info.Width != (uint)TargetWidth || info.Height != (uint)TargetHeight) throw new InvalidOperationException("NC01_SURFACE_SIZE_MISMATCH");
            if (!_lastBefore.StrictEquals(_lastAfter)) throw new InvalidOperationException("NC01_SNAPSHOT_MISMATCH");
            return new NC01CaptureSampleV1
            {
                Eye = eye, Bgra = bgra, SurfaceInfo = StructToBytes(info), ActualViewMatrix = actual16,
                FovY = fovY, AspectRatio = aspect, Gaze = gaze, Ortho = ortho,
                SurfaceRenderElapsedMs = sw.Elapsed.TotalMilliseconds,
                ReadbackQpcTicks = info.ReadbackQpcTicks, CopyQpcTicks = info.CopyQpcTicks,
                OwnedStageSnapshot = _eyeStageSnapshots[eye]
            };
        }

        // Pair core: caller has validated Ready/main invariance and owns the whole busy guard; this path never calls MainSnapshot/TryGetReady nor mutates main, settings or world.
        private NC01CaptureSampleV1 CaptureEyeForPairCore(int eye, Matrix worldFromCamera)
        {
            Stopwatch cameraSw = Stopwatch.StartNew();
            EnsureEyeStagesMatchBaseline(eye);
            IntPtr sceneBefore = RequireScenePointer(eye);
            ICamera cam = _eyes[eye].Camera;
            Matrix wantView = worldFromCamera.Inverse();
            double[] want16 = ToMat16(wantView);
            for (int i = 0; i < 16; i++)
                if (double.IsNaN(want16[i]) || double.IsInfinity(want16[i])) throw new InvalidOperationException("NC01_PAIR_EXPECTED_VIEW_NOT_FINITE:index=" + i.ToString(CultureInfo.InvariantCulture) + "|value=" + FormatInvariant(want16[i]));
            cam.ViewMatrix = wantView;
            double[] actual16 = ToMat16(cam.ViewMatrix);
            if (!MatricesClose(want16, actual16)) throw new InvalidOperationException("NC01_CAMERA_WRITEBACK_MISMATCH");
            double fovY = cam.FieldOfViewY, aspect = cam.AspectRatio, gaze = cam.GazeLength;
            bool ortho = cam.IsOrthographic;
            cameraSw.Stop();

            EyePairReadbackV1 rb = RenderAndReadbackEyePairLocked(eye, sceneBefore);
            return new NC01CaptureSampleV1
            {
                Eye = eye, Bgra = rb.Bgra, SurfaceInfo = StructToBytes(rb.Info), ActualViewMatrix = actual16,
                FovY = fovY, AspectRatio = aspect, Gaze = gaze, Ortho = ortho,
                SurfaceRenderElapsedMs = rb.SurfaceRenderMs,
                ReadbackQpcTicks = rb.Info.ReadbackQpcTicks, CopyQpcTicks = rb.Info.CopyQpcTicks,
                OwnedStageSnapshot = _eyeStageSnapshots[eye],
                EyeCameraMs = cameraSw.Elapsed.TotalMilliseconds,
                EyeLockWaitMs = rb.LockWaitMs,
                EyeStageValidationMs = rb.StageValidationMs,
                EyeAllocateMs = rb.AllocateMs,
                EyeReadbackCallMs = rb.ReadbackCallMs,
                EyeUnlockMs = rb.UnlockMs
            };
        }

        private struct EyePairReadbackV1
        {
            public byte[] Bgra;
            public NC01SurfaceInfoV1 Info;
            public double LockWaitMs, SurfaceRenderMs, StageValidationMs, AllocateMs, ReadbackCallMs, UnlockMs;
        }

        // Shared synchronous lock window used by BOTH pair paths: TryLock -> SurfaceRender -> scene/stage validation ->
        // fresh buffer -> NC01_ReadSurface -> Unlock (always). Negative SurfaceRender rc rejects before any surface read;
        // rc/ABI64B/info/dimension validation runs before the outcome is returned. A new BGRA buffer is allocated per call.
        private EyePairReadbackV1 RenderAndReadbackEyePairLocked(int eye, IntPtr sceneBefore)
        {
            EyePairReadbackV1 outcome = default(EyePairReadbackV1);
            Stopwatch lockSw = Stopwatch.StartNew();
            bool locked = _images[eye].TryLock(new Duration(TimeSpan.FromMilliseconds(10)));
            lockSw.Stop();
            if (!locked) throw new InvalidOperationException("NC01_DX_LOCK_TIMEOUT");

            Stopwatch renderSw = null, stageSw = null, allocSw = null, readSw = null, unlockSw = null;
            byte[] bgra;
            NC01SurfaceInfoV1 info = default(NC01SurfaceInfoV1);
            int rc;
            try
            {
                renderSw = Stopwatch.StartNew();
                int renderRc = _app.SurfaceRender(_eyes[eye]);
                renderSw.Stop();
                // Official SDK treats a negative SurfaceRender return as failure; never read back a stale surface.
                if (renderRc < 0) throw new InvalidOperationException("NC01_SURFACE_RENDER_FAILED:actualrc=" + renderRc.ToString(CultureInfo.InvariantCulture));
                stageSw = Stopwatch.StartNew();
                if (RequireScenePointer(eye) != sceneBefore) throw new InvalidOperationException("NC01_SCENE_IDENTITY_CHANGED");
                EnsureEyeStagesMatchBaseline(eye);
                stageSw.Stop();
                allocSw = Stopwatch.StartNew();
                bgra = new byte[TargetWidth * TargetHeight * 4];
                allocSw.Stop();
                // Window spans the whole helper call (staging/GetRT/LockRect/copy/releases), not GPU duration.
                readSw = Stopwatch.StartNew();
                rc = NC01_ReadSurface(sceneBefore, (uint)TargetWidth, (uint)TargetHeight, bgra,
                    (ulong)bgra.LongLength, ref info, (uint)Marshal.SizeOf(typeof(NC01SurfaceInfoV1)));
                readSw.Stop();
            }
            finally
            {
                unlockSw = Stopwatch.StartNew();
                _images[eye].Unlock();
                unlockSw.Stop();
            }
            if (rc != 0) throw new InvalidOperationException("NC01_READBACK_FAILED:" + rc.ToString(CultureInfo.InvariantCulture));
            if (info.StructBytes != 64u || info.Version != 1u) throw new InvalidOperationException("NC01_ABI_MISMATCH");
            if (info.Width != (uint)TargetWidth || info.Height != (uint)TargetHeight) throw new InvalidOperationException("NC01_SURFACE_SIZE_MISMATCH");
            outcome.Bgra = bgra;
            outcome.Info = info;
            outcome.LockWaitMs = lockSw.Elapsed.TotalMilliseconds;
            outcome.SurfaceRenderMs = renderSw.Elapsed.TotalMilliseconds;
            outcome.StageValidationMs = stageSw.Elapsed.TotalMilliseconds;
            outcome.AllocateMs = allocSw.Elapsed.TotalMilliseconds;
            outcome.ReadbackCallMs = readSw.Elapsed.TotalMilliseconds;
            outcome.UnlockMs = unlockSw.Elapsed.TotalMilliseconds;
            return outcome;
        }

        // Raw-view eye core: the supplied WORLD-TO-CAMERA matrix is written DIRECTLY to the owned eye camera (never Inverse),
        // together with IsOrthographic=false, AspectRatio and the per-eye source FOV, then cam.Update(). The full projection
        // is validated after Update AND again after render+readback finished; any mismatch throws and rejects the whole pair.
        // Sample metadata carries the actual post-readback read-back values, not merely the requested parameters.
        private NC01CaptureSampleV1 CaptureEyeRawForPairCore(int eye, Matrix rawView, double[] want16, double wantFovY, double wantAspect)
        {
            Stopwatch cameraSw = Stopwatch.StartNew();
            EnsureEyeStagesMatchBaseline(eye);
            IntPtr sceneBefore = RequireScenePointer(eye);
            ICamera cam = _eyes[eye].Camera;
            cam.IsOrthographic = false;
            cam.AspectRatio = wantAspect;
            cam.FieldOfViewY = wantFovY;
            cam.ViewMatrix = rawView;
            cam.Update();
            double[] actual16; double actualFovY; double actualAspect; bool actualOrtho;
            ValidateRawEyeProjection(eye, cam, want16, wantFovY, wantAspect, "post-update", out actual16, out actualFovY, out actualAspect, out actualOrtho);
            cameraSw.Stop();

            EyePairReadbackV1 rb = RenderAndReadbackEyePairLocked(eye, sceneBefore);

            ValidateRawEyeProjection(eye, cam, want16, wantFovY, wantAspect, "post-readback", out actual16, out actualFovY, out actualAspect, out actualOrtho);
            double gaze = cam.GazeLength;

            return new NC01CaptureSampleV1
            {
                Eye = eye, Bgra = rb.Bgra, SurfaceInfo = StructToBytes(rb.Info), ActualViewMatrix = actual16,
                FovY = actualFovY, AspectRatio = actualAspect, Gaze = gaze, Ortho = actualOrtho,
                SurfaceRenderElapsedMs = rb.SurfaceRenderMs,
                ReadbackQpcTicks = rb.Info.ReadbackQpcTicks, CopyQpcTicks = rb.Info.CopyQpcTicks,
                OwnedStageSnapshot = _eyeStageSnapshots[eye],
                EyeCameraMs = cameraSw.Elapsed.TotalMilliseconds,
                EyeLockWaitMs = rb.LockWaitMs,
                EyeStageValidationMs = rb.StageValidationMs,
                EyeAllocateMs = rb.AllocateMs,
                EyeReadbackCallMs = rb.ReadbackCallMs,
                EyeUnlockMs = rb.UnlockMs
            };
        }

        // Short raw-projection validation helper: re-reads the FULL owned-eye projection and compares every one of the
        // 16 view entries, FOV, aspect, ortho flag and eye-window dimensions (all 1024-squared) with the narrow absolute
        // RawProjectionTolerance recorded above. `stage` distinguishes post-update from post-readback rejection points.
        private void ValidateRawEyeProjection(int eye, ICamera cam, double[] wantView16, double wantFovY, double wantAspect, string stage,
            out double[] actual16, out double actualFovY, out double actualAspect, out bool actualOrtho)
        {
            actual16 = ToMat16(cam.ViewMatrix);
            actualFovY = cam.FieldOfViewY;
            actualAspect = cam.AspectRatio;
            actualOrtho = cam.IsOrthographic;
            string eyeTag = "|eye=" + eye.ToString(CultureInfo.InvariantCulture) + "|stage=" + stage;
            for (int i = 0; i < 16; i++)
                if (!RawClose(wantView16[i], actual16[i]))
                    throw new InvalidOperationException("NC01_RAW_VIEW_WRITEBACK_MISMATCH" + eyeTag
                        + "|index=" + i.ToString(CultureInfo.InvariantCulture)
                        + "|expected=" + FormatInvariant(wantView16[i]) + "|actual=" + FormatInvariant(actual16[i])
                        + "|tolerance=" + RawProjectionTolerance.ToString("R", CultureInfo.InvariantCulture));
            if (actualOrtho) throw new InvalidOperationException("NC01_RAW_ORTHO_MISMATCH" + eyeTag + "|actualOrtho=1|expectedOrtho=0");
            if (!RawClose(actualFovY, wantFovY))
                throw new InvalidOperationException("NC01_RAW_FOV_MISMATCH" + eyeTag + "|expected=" + FormatInvariant(wantFovY)
                    + "|actual=" + FormatInvariant(actualFovY) + "|tolerance=" + RawProjectionTolerance.ToString("R", CultureInfo.InvariantCulture));
            if (!RawClose(actualAspect, wantAspect))
                throw new InvalidOperationException("NC01_RAW_ASPECT_MISMATCH" + eyeTag + "|expected=" + FormatInvariant(wantAspect)
                    + "|actual=" + FormatInvariant(actualAspect) + "|tolerance=" + RawProjectionTolerance.ToString("R", CultureInfo.InvariantCulture));
            double ownedWidth = _eyes[eye].Width, ownedHeight = _eyes[eye].Height;
            if (!RawClose(ownedWidth, TargetWidth) || !RawClose(ownedHeight, TargetHeight))
                throw new InvalidOperationException("NC01_RAW_OWNED_DIMS_MISMATCH" + eyeTag
                    + "|width=" + FormatInvariant(ownedWidth) + "|height=" + FormatInvariant(ownedHeight)
                    + "|expected=" + TargetWidth.ToString(CultureInfo.InvariantCulture) + "x" + TargetHeight.ToString(CultureInfo.InvariantCulture));
        }

        // Whole-pair capture under one busy guard: exactly two MainSnapshot calls on success (before + after), one CaptureEyeForPairCore per eye, no partial pair on stop or error.
        public NC01CaptureSampleV1[] CapturePair(Matrix leftWorld, Matrix rightWorld, Func<bool> shouldStop)
        {
            // Runtime instances refuse this legacy world-matrix entry outright BEFORE any SDK getter/setter or busy
            // registration: it cannot honor the generation contract. Runtime pairs use CaptureRawPair(generation) only.
            // The full non-runtime body and pair timing are unchanged.
            if (_runtimeBackend) throw new InvalidOperationException("NC01_RUNTIME_LEGACY_SDK_ENTRY_REJECTED:capturePair|boundGeneration=" + FormatInvariant(Volatile.Read(ref _boundGeneration)));
            VerifyAccess();
            if (shouldStop == null) throw new ArgumentNullException("shouldStop");
            if (_hostingState != 2) throw new InvalidOperationException("NC01_NOT_HOSTED");
            if (_captureDisabled) throw new InvalidOperationException("NC01_CAPTURE_DISABLED:" + _disabledReason);
            if (Interlocked.CompareExchange(ref _captureBusy, 1, 0) != 0) throw new InvalidOperationException("NC01_CAPTURE_BUSY");
            try { return CapturePairCore(leftWorld, rightWorld, shouldStop); }
            catch
            {
                if (!_captureDisabled) DisableCaptures("NC01_PAIR_CAPTURE_FAULT");
                throw;
            }
            finally { Interlocked.Exchange(ref _captureBusy, 0); }
        }

        private NC01CaptureSampleV1[] CapturePairCore(Matrix leftWorld, Matrix rightWorld, Func<bool> shouldStop)
        {
            Stopwatch prepareSw = Stopwatch.StartNew();
            if (shouldStop()) throw new OperationCanceledException("NC02_PAIR_STOP_REQUESTED");
            NC01MainSnapshotV1 before = MainSnapshotCore();
            if (!before.StrictEquals(_baseline)) { DisableCaptures("NC01_PAIR_BASELINE_DRIFT"); throw new InvalidOperationException(_disabledReason); }
            string readyReason;
            if (!TryEyeReady(0, out readyReason) || !TryConfirmEyeStageBaseline(0, out readyReason))
                throw new InvalidOperationException("NC01_PAIR_PREPARE_REFUSED:eye=0|reason=" + readyReason);
            if (!TryEyeReady(1, out readyReason) || !TryConfirmEyeStageBaseline(1, out readyReason))
                throw new InvalidOperationException("NC01_PAIR_PREPARE_REFUSED:eye=1|reason=" + readyReason);
            double prepareMs = prepareSw.Elapsed.TotalMilliseconds;
            if (shouldStop()) throw new OperationCanceledException("NC02_PAIR_STOP_REQUESTED");
            NC01CaptureSampleV1 left = CaptureEyeForPairCore(0, leftWorld);
            double mark = prepareSw.Elapsed.TotalMilliseconds;
            if (shouldStop()) throw new OperationCanceledException("NC02_PAIR_STOP_REQUESTED");
            prepareMs += prepareSw.Elapsed.TotalMilliseconds - mark;
            NC01CaptureSampleV1 right = CaptureEyeForPairCore(1, rightWorld);
            mark = prepareSw.Elapsed.TotalMilliseconds;
            if (shouldStop()) throw new OperationCanceledException("NC02_PAIR_STOP_REQUESTED");
            prepareMs += prepareSw.Elapsed.TotalMilliseconds - mark;
            prepareSw.Stop();
            Stopwatch finalizeSw = Stopwatch.StartNew();
            NC01MainSnapshotV1 after = MainSnapshotCore();
            bool invariant = after.StrictEquals(before) && after.StrictEquals(_baseline);
            finalizeSw.Stop();
            if (!invariant) { DisableCaptures("NC01_PAIR_SNAPSHOT_MISMATCH"); throw new InvalidOperationException(_disabledReason); }
            _lastBefore = before;
            _lastAfter = after;
            double finalizeMs = finalizeSw.Elapsed.TotalMilliseconds;
            left.PairPrepareMs = right.PairPrepareMs = prepareMs;
            left.PairFinalizeMs = right.PairFinalizeMs = finalizeMs;
            left.PairMainBeforeSnapshot = right.PairMainBeforeSnapshot = before.Serialized;
            left.PairMainAfterSnapshot = right.PairMainAfterSnapshot = after.Serialized;
            return new NC01CaptureSampleV1[] { left, right };
        }

        // Runtime-only raw-view pair API. leftRawView/rightRawView are WORLD-TO-CAMERA view matrices by contract and are
        // written directly as the owned cameras' ViewMatrix; this API NEVER Inverses them. The existing CapturePair
        // (world,world,shouldStop) contract — one Inverse at the current boundary — is unchanged. Every input is gated
        // BEFORE any SDK getter/setter: both matrices fully finite (16/16), each FOV finite and 0<deg<180, sourceAspect
        // finite positive and equal to 1024/1024 within RawProjectionTolerance, perspective must be true, shouldStop
        // non-null, the owner must be the runtime backend, and (generation overload) generation must equal the positively
        // bound generation with the admission gate still open. Returns only when BOTH eyes fully succeed; samples carry
        // the actual post-readback projection/view-matrix metadata.
        // Legacy overload without generation: forwards to the already-bound runtime generation or refuses clearly while
        // unbound — it is not an escape hatch around the admission gate.
        public NC01CaptureSampleV1[] CaptureRawPair(Matrix leftRawView, Matrix rightRawView, double leftSourceFovYDeg, double rightSourceFovYDeg, double sourceAspect, bool perspective, Func<bool> shouldStop)
        {
            int bound = BoundGeneration;
            if (_runtimeBackend && bound <= 0) throw new InvalidOperationException("NC01_RAW_PAIR_GENERATION_NOT_BOUND:legacyOverload");
            return CaptureRawPair(leftRawView, rightRawView, leftSourceFovYDeg, rightSourceFovYDeg, sourceAspect, perspective, bound, shouldStop);
        }

        public NC01CaptureSampleV1[] CaptureRawPair(Matrix leftRawView, Matrix rightRawView, double leftSourceFovYDeg, double rightSourceFovYDeg, double sourceAspect, bool perspective, int generation, Func<bool> shouldStop)
        {
#if VRSIM_SOURCE_4096_QA
            if (TargetWidth == 4096 && TargetHeight == 4096)
                throw new InvalidOperationException("NC01_SOURCE4096_CPU_READBACK_REFUSED");
#endif
#if VRSIM_SHARED_3072
            // The frozen 3072x3072 geometry is GPU-seam-only: refuse the CPU read-back route here, as the
            // very first action of this overload — before the generation check, any SDK Matrix indexer read,
            // VerifyAccess, busy registration, byte[] allocation, DisableCaptures or shouldStop contact. A
            // plain throw; nothing sticky is written.
            if (TargetWidth == 3072 && TargetHeight == 3072)
                throw new InvalidOperationException("NC01_SHARED3072_CPU_READBACK_REFUSED");
#endif
            if (shouldStop == null) throw new ArgumentNullException("shouldStop");
            if (!_runtimeBackend) throw new InvalidOperationException("NC01_RAW_PAIR_RUNTIME_BACKEND_REQUIRED:runtimeBackend=0");
            if (generation <= 0) throw new ArgumentOutOfRangeException("generation");
            // Primitive (pure double/bool/struct) input validation runs first: it touches no SDK state, and an invalid
            // input must reject WITHOUT registering busy or writing any sticky owner fault.
            if (!perspective) throw new InvalidOperationException("NC01_RAW_PAIR_PERSPECTIVE_REQUIRED:perspective=0");
            RequireRawFovY(0, leftSourceFovYDeg);
            RequireRawFovY(1, rightSourceFovYDeg);
            RequireRawSourceAspect(sourceAspect);
            VerifyAccess();
            // Gate snapshot: original-generation match, permanent stop, admission, hosting, sticky fault and the required
            // frozen quality state are checked, and BOTH busy registrations (logical call + capture) are taken, under the
            // same short lock. There is no window where a concurrent RequestStop could report completion while this call
            // proceeds to its SDK reads, and the captureBusy hand-off leaves no Stop-visible busy=0 gap. UI re-entry is
            // refused by the busy flags with plain throws — no sticky FailureReason write.
            lock (_gateLock)
            {
                int bound = Volatile.Read(ref _boundGeneration);
                if (bound != generation) throw new InvalidOperationException("NC01_RAW_PAIR_GENERATION_MISMATCH:bound=" + FormatInvariant(bound) + "|requested=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _stopRequested) != 0) throw new OperationCanceledException("NC02_RAW_PAIR_STOP_REQUESTED:generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _admissionOpen) == 0) throw new InvalidOperationException("NC01_RAW_PAIR_ADMISSION_CLOSED:generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (Volatile.Read(ref _hostingState) != 2) throw new InvalidOperationException("NC01_NOT_HOSTED");
                if (_captureDisabled) throw new InvalidOperationException("NC01_CAPTURE_DISABLED:" + _disabledReason);
                // Refuse BEFORE the busy registration while the managed quality fingerprint is still pending (generation 0);
                // this is a plain rejection, not a drift, so it must not sticky-disable the lease.
                if (_qualityGeneration != 1) throw new InvalidOperationException("NC01_RAW_PAIR_QUALITY_NOT_FROZEN:generation=" + _qualityGeneration.ToString(CultureInfo.InvariantCulture));
                if (_runtimeCallsBusy != 0) throw new InvalidOperationException("NC01_RUNTIME_CALL_BUSY:rawPair|generation=" + generation.ToString(CultureInfo.InvariantCulture));
                if (_captureBusy != 0) throw new InvalidOperationException("NC01_CAPTURE_BUSY");
                _runtimeCallsBusy = 1;
                _captureBusy = 1;
            }
            try
            {
                // The SDK Matrix indexer reads happen ONLY after VerifyAccess, the logical busy registration above and a
                // dedicated grant group for this registered logical call; a non-finite matrix rejects as a plain input
                // refusal outside the capture-fault handler and must never become a sticky capture fault. A stop/same-
                // lease-close refusal here propagates as OperationCanceledException without touching any sticky state.
                RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_INPUT_MATRIX", true, "NC02_RAW_PAIR_STOP_REQUESTED");
                double[] left16 = RequireRawView16Finite(0, leftRawView);
                double[] right16 = RequireRawView16Finite(1, rightRawView);
                try { return CaptureRawPairCore(generation, leftRawView, rightRawView, left16, right16, leftSourceFovYDeg, rightSourceFovYDeg, sourceAspect, shouldStop); }
                catch (OperationCanceledException) { throw; }
                catch
                {
                    if (!_captureDisabled) DisableCaptures("NC01_RAW_PAIR_CAPTURE_FAULT");
                    throw;
                }
            }
            // Every registered path returns BOTH busy flags under the same gate lock, including early refusals after
            // registration, pending outcomes and exceptions; the shouldStop callback runs only outside the lock.
            finally { lock (_gateLock) { Volatile.Write(ref _captureBusy, 0); Volatile.Write(ref _runtimeCallsBusy, 0); } }
        }

        // Whole-pair raw capture reusing the same guard flow as CapturePairCore: exactly two MainSnapshotCore calls on success,
        // per-eye readiness/stage baselines and a generation/admission/shouldStop locked grant before EVERY group (input-matrix
        // finite reads, prepare snapshot, each eye-ready group, the quality-build group, each full raw eye render group, the
        // final snapshot group, the own-eye post gate group and the atomic managed commit). A stop landing after the final
        // snapshot or the post gate but before commit rejects the WHOLE pair — no half pair is returned or stamped.
        private NC01CaptureSampleV1[] CaptureRawPairCore(int generation, Matrix leftRawView, Matrix rightRawView, double[] left16, double[] right16,
            double leftFovY, double rightFovY, double sourceAspect, Func<bool> shouldStop)
        {
            Stopwatch prepareSw = Stopwatch.StartNew();
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_PREPARE", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            VerifyRuntimeIdentityBindings();
            NC01MainSnapshotV1 before = MainSnapshotCore();
            string readyReason;
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_READY0", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            if (!TryEyeReady(0, out readyReason) || !TryConfirmEyeStageBaseline(0, out readyReason))
                throw new InvalidOperationException("NC01_RAW_PAIR_PREPARE_REFUSED:eye=0|reason=" + readyReason);
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_READY1", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            if (!TryEyeReady(1, out readyReason) || !TryConfirmEyeStageBaseline(1, out readyReason))
                throw new InvalidOperationException("NC01_RAW_PAIR_PREPARE_REFUSED:eye=1|reason=" + readyReason);
            // Runtime pairs validate quality from the ALREADY-READ fresh before snapshot string (no extra full snapshot);
            // the whole-session baseline equality requirement is cancelled here: the user may legitimately change the main
            // camera or focus between pairs, while WITHIN a pair before/after must still be strictly identical.
            // The quality builder below performs the actual own-eye SDK display getter reads, so it gets its OWN small
            // grant group right before the sampling instead of riding naked after READY1.
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_QUALITY_BUILD", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            string pairFingerprint, pairBuildError;
            string[] pairSubset;
            if (!TryBuildRuntimeQualityFingerprint(before.Serialized, out pairFingerprint, out pairSubset, out pairBuildError))
            { DisableCaptures(pairBuildError); throw new InvalidOperationException(_disabledReason); }
            if (!string.Equals(_runtimeQualityFingerprint, pairFingerprint, StringComparison.Ordinal))
            { DisableCaptures("NC01_RAW_PAIR_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(pairSubset)); throw new InvalidOperationException(_disabledReason); }
            double prepareMs = prepareSw.Elapsed.TotalMilliseconds;
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_LEFT_EYE", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            NC01CaptureSampleV1 left = CaptureEyeRawForPairCore(0, leftRawView, left16, leftFovY, sourceAspect);
            double mark = prepareSw.Elapsed.TotalMilliseconds;
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_RIGHT_EYE", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            prepareMs += prepareSw.Elapsed.TotalMilliseconds - mark;
            NC01CaptureSampleV1 right = CaptureEyeRawForPairCore(1, rightRawView, right16, rightFovY, sourceAspect);
            mark = prepareSw.Elapsed.TotalMilliseconds;
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_FINAL_SNAPSHOT", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            prepareMs += prepareSw.Elapsed.TotalMilliseconds - mark;
            prepareSw.Stop();
            Stopwatch finalizeSw = Stopwatch.StartNew();
            NC01MainSnapshotV1 after = MainSnapshotCore();
            bool invariant = after.StrictEquals(before);
            finalizeSw.Stop();
            if (!invariant) { DisableCaptures("NC01_RAW_PAIR_SNAPSHOT_MISMATCH"); throw new InvalidOperationException(_disabledReason); }
            // Post-render verification gate: main before/after equality cannot see own-eye stage/display or identity drift
            // introduced during the SDK pump/render, because the quality builder consumes cached stage strings. Re-read the
            // ACTUAL own stages, rebuild the fingerprint from the ALREADY-TAKEN after snapshot (no third MainSnapshot), and
            // re-verify strong identity bindings. This runs in its own admission-guarded group BEFORE the existing commit
            // gate: a stop that landed after the final snapshot cancels here without opening new SDK work and is never
            // recorded as a quality fault, and the rest of the gate is still rejected by the commit gate below. The frozen
            // _runtimeQualityFingerprint and QualityGeneration are never updated by this gate.
            RequireRuntimeGroup(generation, shouldStop, "NC01_RAW_PAIR_GROUP_POST_GATE", true, "NC02_RAW_PAIR_STOP_REQUESTED");
            EnsureEyeStagesMatchBaseline(0);
            EnsureEyeStagesMatchBaseline(1);
            string postFingerprint, postBuildError;
            string[] postSubset;
            if (!TryBuildRuntimeQualityFingerprint(after.Serialized, out postFingerprint, out postSubset, out postBuildError))
            { DisableCaptures(postBuildError); throw new InvalidOperationException(_disabledReason); }
            if (!string.Equals(_runtimeQualityFingerprint, postFingerprint, StringComparison.Ordinal))
            { DisableCaptures("NC01_RAW_PAIR_QUALITY_DRIFT" + DescribeRuntimeQualityDrift(postSubset)); throw new InvalidOperationException(_disabledReason); }
            VerifyRuntimeIdentityBindings();
            // Atomic managed pair commit. The final shouldStop runs OUTSIDE the gate lock first and may re-enter
            // RequestStop while returning false; the locked re-verification still refuses. Re-verification of the
            // original generation, admission, permanent stop, fault and busy plus the COMPLETE managed publish
            // (_lastBefore/_lastAfter, both eyes' shared metadata and the returned pair) happen on ONE gate snapshot:
            // a stop winning before this commit rejects the WHOLE pair with OperationCanceledException and stamps
            // nothing, and the commit can never publish only one eye. A stop winning after the commit closes the lease
            // normally while the producer keeps its own post-generation/admission discard gate.
            if (shouldStop()) throw new OperationCanceledException("NC02_RAW_PAIR_STOP_REQUESTED:group=NC01_RAW_PAIR_COMMIT|generation=" + generation.ToString(CultureInfo.InvariantCulture));
            bool pairCommitStopCancel;
            string pairCommitRejectReason;
            bool pairCommitted;
            double pairFinalizeMs = finalizeSw.Elapsed.TotalMilliseconds;
            lock (_gateLock)
            {
                pairCommitted = RuntimeGroupGrant(generation, "NC01_RAW_PAIR_COMMIT", true, out pairCommitStopCancel, out pairCommitRejectReason);
                if (pairCommitted)
                {
                    _lastBefore = before;
                    _lastAfter = after;
                    left.PairPrepareMs = right.PairPrepareMs = prepareMs;
                    left.PairFinalizeMs = right.PairFinalizeMs = pairFinalizeMs;
                    left.PairMainBeforeSnapshot = right.PairMainBeforeSnapshot = before.Serialized;
                    left.PairMainAfterSnapshot = right.PairMainAfterSnapshot = after.Serialized;
                }
            }
            if (!pairCommitted)
            {
                if (pairCommitStopCancel) throw new OperationCanceledException("NC02_RAW_PAIR_STOP_REQUESTED:group=NC01_RAW_PAIR_COMMIT|generation=" + generation.ToString(CultureInfo.InvariantCulture) + "|recheck=1");
                throw new InvalidOperationException(pairCommitRejectReason);
            }
            return new NC01CaptureSampleV1[] { left, right };
        }

        private bool TryEyeReady(int eye, out string reason)
        {
            reason = null;
            IWindow3D window = _eyes[eye];
            object view = _views[eye];
            if (view == null)
            {
                object candidate;
                try { candidate = ReflectRequireGetter(_vms[eye], "Direct3DViewportView"); }
                catch (InvalidOperationException ex) { DisableCaptures(ex.Message); reason = _disabledReason; return false; }
                if (candidate == null) { reason = "NC01_VIEW_NOT_LOADED"; return false; }
                if (!(candidate is FrameworkElement))
                { DisableCaptures("NC01_VIEW_NOT_FRAMEWORK_ELEMENT:actualtype=" + candidate.GetType().FullName); reason = _disabledReason; return false; }
                _views[eye] = candidate;
                view = candidate;
            }
            if (_images[eye] == null)
            {
                object raw;
                try { raw = ReflectRequireField(view, "d3dimg"); }
                catch (InvalidOperationException ex) { DisableCaptures(ex.Message); reason = _disabledReason; return false; }
                if (raw == null) { reason = "NC01_D3DIMAGE_NOT_READY"; return false; }
                D3DImage image = raw as D3DImage;
                if (image == null)
                { DisableCaptures("NC01_D3DIMAGE_TYPE_UNEXPECTED:actualtype=" + raw.GetType().FullName); reason = _disabledReason; return false; }
                _images[eye] = image;
                FrameworkElement pending = (FrameworkElement)view;
                pending.IsHitTestVisible = false; pending.Focusable = false;
                pending.Width = TargetWidth / _scaleX; pending.Height = TargetHeight / _scaleY;
                reason = "NC01_LAYOUT_PENDING";
                return false;
            }
            object currentView = ReflectRequireGetter(_vms[eye], "Direct3DViewportView");
            if (!ReferenceEquals(currentView, view)) { DisableCaptures("NC01_VIEW_INSTANCE_CHANGED"); reason = _disabledReason; return false; }
            FrameworkElement el = (FrameworkElement)view;
            object bound = ReflectRequireField(view, "_window");
            if (!ReferenceEquals(bound, window) && !Equals(bound, window)) { DisableCaptures("NC01_FOREIGN_WINDOW"); reason = _disabledReason; return false; }
            if (!IsClose(window.Width, TargetWidth) || !IsClose(window.Height, TargetHeight) || !el.IsLoaded)
            {
                // Failure-only evidence for the frozen per-owner target DIP layout; never per-frame logging.
                reason = "NC01_LAYOUT_NOT_STABLE"
                    + "|expected=" + TargetWidth.ToString(CultureInfo.InvariantCulture) + "x" + TargetHeight.ToString(CultureInfo.InvariantCulture)
                    + "|sdk=(" + FormatInvariant(window.Width) + "," + FormatInvariant(window.Height) + ")"
                    + "|elWidth=" + FormatInvariant(el.Width) + "|elHeight=" + FormatInvariant(el.Height)
                    + "|elActual=(" + FormatInvariant(el.ActualWidth) + "," + FormatInvariant(el.ActualHeight) + ")"
                    + "|isLoaded=" + Bool(el.IsLoaded)
                    + "|dpi=(" + FormatInvariant(_scaleX) + "," + FormatInvariant(_scaleY) + ")"
                    + "|d3dPixels=" + _images[eye].PixelWidth.ToString(CultureInfo.InvariantCulture) + "x" + _images[eye].PixelHeight.ToString(CultureInfo.InvariantCulture);
                return false;
            }
            // Actual D3DImage back-buffer must match the frozen target; scene/readback checks below stay the
            // authoritative gate. A mismatch is a plain refusal (no force-fix, no sticky write, no dispose/reopen).
            if (_images[eye].PixelWidth != TargetWidth || _images[eye].PixelHeight != TargetHeight)
            {
                reason = "NC01_D3DIMAGE_SIZE_MISMATCH"
                    + "|expected=" + TargetWidth.ToString(CultureInfo.InvariantCulture) + "x" + TargetHeight.ToString(CultureInfo.InvariantCulture)
                    + "|actual=" + _images[eye].PixelWidth.ToString(CultureInfo.InvariantCulture) + "x" + _images[eye].PixelHeight.ToString(CultureInfo.InvariantCulture);
                return false;
            }
            if (!((bool)ReflectRequireGetter(_vms[eye], "IsDXReady"))) { reason = "NC01_DX_NOT_READY"; return false; }
            _layoutEvidence[eye] = string.Format(CultureInfo.InvariantCulture,
                "eye={0} dpi=({1:R},{2:R}) dip=({3:R},{4:R}) actual=({5:R},{6:R}) sdkPixels=({7:R},{8:R})",
                eye, _scaleX, _scaleY, el.Width, el.Height, el.ActualWidth, el.ActualHeight,
                el.ActualWidth * _scaleX, el.ActualHeight * _scaleY);
            if (ScenePointerForRead(eye) == IntPtr.Zero) { reason = "NC01_SCENE_NOT_READY"; return false; }
            if (!_eyeDisplayCopied[eye])
            {
                // IsDXReady alone does not prove the eye's own traverser; a non-empty target RenderStages snapshot is the evidence that field setters are live.
                if (ReadRenderStagesOrNullWhenEmpty(window) == null) { reason = "NC01_WAITING_OWN_RENDERER_INIT"; return false; }
                CopyDisplayFields(window);
                _eyeDisplayCopied[eye] = true;
                reason = "NC01_DISPLAY_COPIED";
                return false;
            }
            return true;
        }

        private IntPtr RequireScenePointer(int eye)
        {
            IntPtr scene = ScenePointerForRead(eye);
            if (scene == IntPtr.Zero) throw new InvalidOperationException("NC01_SCENE_NULL");
            return scene;
        }

        private IntPtr ScenePointerForRead(int eye)
        {
            object scene = ReflectRequireField(_views[eye], "_scene");
            if (scene == null) return IntPtr.Zero;
            if (scene is IntPtr) return (IntPtr)scene;
            PropertyInfo handle = FindProperty(scene.GetType(), "Handle");
            object value = handle != null && handle.CanRead ? handle.GetValue(scene, null) : null;
            if (!(value is IntPtr)) throw new InvalidOperationException("NC01_SCENE_REF_UNSUPPORTED:" + scene.GetType().FullName);
            return (IntPtr)value;
        }

        private void EnsureEyeStagesMatchBaseline(int eye)
        {
            if (!string.Equals(_eyeStageSnapshots[eye], ReadRenderStages(_eyes[eye]), StringComparison.Ordinal))
            { DisableCaptures("NC01_EYE_STAGE_DRIFT"); throw new InvalidOperationException(_disabledReason); }
        }

        private void CopyDisplayFields(IWindow3D eyeWindow)
        {
            IWindow3D main = _mainWindow;
            CopyHeadlightField(main, eyeWindow);
            CopyDisplayField("EdgeShadingMode", () => main.EdgeShadingMode, v => eyeWindow.EdgeShadingMode = v, () => eyeWindow.EdgeShadingMode);
            CopyDisplayField("EdgeColorMode", () => main.EdgeColorMode, v => eyeWindow.EdgeColorMode = v, () => eyeWindow.EdgeColorMode);
            CopyDisplayField("RenderShadingMode", () => main.RenderShadingMode, v => eyeWindow.RenderShadingMode = v, () => eyeWindow.RenderShadingMode);
            CopyDisplayField("UseMaterialShaders", () => main.UseMaterialShaders, v => eyeWindow.UseMaterialShaders = v, () => eyeWindow.UseMaterialShaders);
            CopyFloatDisplayField("RenderOpacity", () => main.RenderOpacity, v => eyeWindow.RenderOpacity = v, () => eyeWindow.RenderOpacity);
            CopyColorDisplayField("BackgroundColor", () => main.BackgroundColor, v => eyeWindow.BackgroundColor = v, () => eyeWindow.BackgroundColor, c => ColorRgba4(c));
            CopyDisplayField("TransparencyQuality", () => main.TransparencyQuality, v => eyeWindow.TransparencyQuality = v, () => eyeWindow.TransparencyQuality);
        }

        private static void CopyHeadlightField(IWindow3D main, IWindow3D eyeWindow)
        {
            try
            {
                bool target = main.UseHeadlight;
                bool current = eyeWindow.UseHeadlight;
                if (current == target) return;
                // The false setter mutates the shared WorldSunlightDirection; refuse instead of invoking it.
                if (!target) throw new InvalidOperationException("NC01_SHARED_LIGHT_MUTATION_UNSUPPORTED:eyeUseHeadlight=" + Bool(current) + "|mainUseHeadlight=" + Bool(target));
                eyeWindow.UseHeadlight = true;
                if (eyeWindow.UseHeadlight) return;
                throw FieldCopyMismatch("UseHeadlight", true, eyeWindow.UseHeadlight);
            }
            catch (InvalidOperationException) { throw; }
            catch (Exception ex) { throw FieldCopyException("UseHeadlight", ex); }
        }

        private static void CopyDisplayField<T>(string fieldName, Func<T> readMain, Action<T> writeEye, Func<T> readEye)
        {
            try
            {
                T target = readMain();
                if (Equals(readEye(), target)) return;
                writeEye(target);
                T after = readEye();
                if (Equals(after, target)) return;
                throw FieldCopyMismatch(fieldName, target, after);
            }
            catch (InvalidOperationException) { throw; }
            catch (Exception ex) { throw FieldCopyException(fieldName, ex); }
        }

        private static void CopyFloatDisplayField(string fieldName, Func<float> readMain, Action<float> writeEye, Func<float> readEye)
        {
            try
            {
                float target = readMain();
                if (IsClose(readEye(), target)) return;
                writeEye(target);
                float after = readEye();
                if (IsClose(after, target)) return;
                throw FieldCopyMismatch(fieldName, target, after);
            }
            catch (InvalidOperationException) { throw; }
            catch (Exception ex) { throw FieldCopyException(fieldName, ex); }
        }

        private static void CopyColorDisplayField<TColor>(string fieldName, Func<TColor> readMain, Action<TColor> writeEye, Func<TColor> readEye, Func<TColor, string> rgba4)
        {
            try
            {
                TColor target = readMain();
                string want = rgba4(target);
                if (string.Equals(want, rgba4(readEye()), StringComparison.Ordinal)) return;
                writeEye(target);
                string actual = rgba4(readEye());
                if (string.Equals(want, actual, StringComparison.Ordinal)) return;
                throw new InvalidOperationException("NC01_DISPLAY_FIELD_COPY_FAILED:" + fieldName + "|expectedRgba4=" + want + "|actualRgba4=" + actual);
            }
            catch (InvalidOperationException) { throw; }
            catch (Exception ex) { throw FieldCopyException(fieldName, ex); }
        }

        private static string ColorRgba4(object color)
        {
            if (color == null) return "<null>";
            return string.Concat(
                FormatInvariant(ReflectRequireGetter(color, "Red")), ",",
                FormatInvariant(ReflectRequireGetter(color, "Green")), ",",
                FormatInvariant(ReflectRequireGetter(color, "Blue")), ",",
                FormatInvariant(ReflectRequireGetter(color, "Alpha")));
        }

        private static InvalidOperationException FieldCopyMismatch(string fieldName, object expected, object actual)
        {
            return new InvalidOperationException("NC01_DISPLAY_FIELD_COPY_FAILED:" + fieldName + "|expected=" + FormatInvariant(expected) + "|actual=" + FormatInvariant(actual));
        }

        private static InvalidOperationException FieldCopyException(string fieldName, Exception ex)
        {
            return new InvalidOperationException("NC01_DISPLAY_FIELD_COPY_FAILED:" + fieldName + "|originaltype=" + ex.GetType().FullName + "|originalmessage=" + ex.Message, ex);
        }

        private static readonly string[] QualityFieldNames = {
            "IsAntialiasingEnabled", "MultisamplingMode", "IsShadowsEnabled", "ShadowSoftness",
            "IsMaterialShaderEnabled", "IsEffectShaderEnabled", "IsAmbientOcclusionEnabled",
            "AmbientOcclusionQuality", "SoftwareFallbackMode" };
        private static readonly string[] ActualRenderFlagNames = BuildActualRenderFlagNames();
        private static string[] BuildActualRenderFlagNames()
        {
            string[] extra = { "RenderingMode", "RenderingPresetMode", "IsRenderingEnabled" };
            string[] all = new string[QualityFieldNames.Length + extra.Length];
            Array.Copy(QualityFieldNames, all, QualityFieldNames.Length);
            Array.Copy(extra, 0, all, QualityFieldNames.Length, extra.Length);
            return all;
        }

        private object ResolveApplicationSettingService()
        {
            object settings = null;
#if VRSIM_QA_D9_BEGIN_POLL
            try
            {
                FieldInfo appSettingsField = FindFieldCached(_mainVp.GetType(), "_appSettings");
                if (appSettingsField != null) settings = appSettingsField.GetValue(_mainVp) as VisualComponents.Create3D.IApplicationSettingService;
            }
            catch (InvalidOperationException) { settings = null; }
#else
            try { settings = ReflectRequireField(_mainVp, "_appSettings") as VisualComponents.Create3D.IApplicationSettingService; }
            catch (InvalidOperationException) { settings = null; }
#endif
            if (settings == null)
            {
                object renderService = ReflectRequireField(_mainVp, "_renderService");
                if (renderService == null) throw new InvalidOperationException("NC01_APP_SETTINGS_UNAVAILABLE");
                settings = ReflectRequireField(renderService, "_applicationSettingService") as VisualComponents.Create3D.IApplicationSettingService;
                if (settings == null) throw new InvalidOperationException("NC01_APP_SETTINGS_UNAVAILABLE");
            }
            return settings;
        }

        private string ReadConfiguredDisplayQuality()
        {
#if VRSIM_QA_D9_BEGIN_POLL
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_OWNER_PHASE_CPU_WALL
            long qaCpuWallT0 = QaConfiguredQualityWallTimestampOrZero();
#endif
            object settings = ResolveApplicationSettingService();
#if VRSIM_QA_OUTPUT_4096 && VRSIM_QA_OWNER_PHASE_CPU_WALL
            long qaCpuWallT1 = QaConfiguredQualityWallTimestampOrZero();
#endif
            MethodInfo getSection;
            lock (_getSectionCacheLock) { getSection = _cachedGetSectionClosedDisplay; }
            if (getSection == null)
            {
                MethodInfo openGetSection = null;
                foreach (MethodInfo m in typeof(VisualComponents.Create3D.IApplicationSettingService).GetMethods())
                    if (m.Name == "GetSection" && m.IsGenericMethodDefinition && m.GetParameters().Length == 0) { openGetSection = m; break; }
                if (openGetSection == null) throw new InvalidOperationException("NC01_GET_SECTION_MISSING");
                try
                {
                    getSection = openGetSection.MakeGenericMethod(typeof(VisualComponents.Create3D.IDisplaySection));
                    lock (_getSectionCacheLock) { _cachedGetSectionClosedDisplay = getSection; }
                }
                catch (Exception ex) { throw new InvalidOperationException("NC01_DISPLAY_SECTION_READ_FAILED", ex); }
            }
            object section;
            try { section = getSection.Invoke(settings, null); }
            catch (Exception ex) { throw new InvalidOperationException("NC01_DISPLAY_SECTION_READ_FAILED", ex); }
#else
            object settings = ResolveApplicationSettingService();
            MethodInfo getSection = null;
            foreach (MethodInfo m in typeof(VisualComponents.Create3D.IApplicationSettingService).GetMethods())
                if (m.Name == "GetSection" && m.IsGenericMethodDefinition && m.GetParameters().Length == 0) { getSection = m; break; }
            if (getSection == null) throw new InvalidOperationException("NC01_GET_SECTION_MISSING");
            object section;
            try { section = getSection.MakeGenericMethod(typeof(VisualComponents.Create3D.IDisplaySection)).Invoke(settings, null); }
            catch (Exception ex) { throw new InvalidOperationException("NC01_DISPLAY_SECTION_READ_FAILED", ex); }
#endif
            if (section == null) throw new InvalidOperationException("NC01_DISPLAY_SECTION_NULL");
#if VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OUTPUT_4096 && VRSIM_QA_OWNER_PHASE_CPU_WALL
            long qaCpuWallT2 = QaConfiguredQualityWallTimestampOrZero();
#endif
            object element = ReflectRequireGetter(section, "DisplayElement");
            if (element == null) throw new InvalidOperationException("NC01_DISPLAY_ELEMENT_NULL");
#if VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OUTPUT_4096 && VRSIM_QA_OWNER_PHASE_CPU_WALL
            string qaCpuWallText = ReadRequiredGetterSnapshot(element, QualityFieldNames, "QUALITY");
            long qaCpuWallT3 = QaConfiguredQualityWallTimestampOrZero();
            CommitConfiguredQualityCpuWallSample(qaCpuWallT0, qaCpuWallT1, qaCpuWallT2, qaCpuWallT3);
            return qaCpuWallText;
#else
            return ReadRequiredGetterSnapshot(element, QualityFieldNames, "QUALITY");
#endif
        }

#if VRSIM_QA_D9_BEGIN_POLL && VRSIM_QA_OUTPUT_4096 && VRSIM_QA_OWNER_PHASE_CPU_WALL
        // Readiness-frozen bound-session snapshot helper for the triple-macro inner MainSnapshotCore ONLY (the caller's
        // VerifyAccess already ran before this). Admission requires the caller's inner flag, quality generation 1, a
        // positive bound generation, admission open and stop not requested; one short gate lock snapshot also captures
        // the existing frozen quality subset reference only, never an SDK call, reflection, formatting or other slow
        // operation. Non-inner/unready/closing invokes the original ReadConfiguredDisplayQuality exactly once OUTSIDE
        // that lock and preserves its exceptions unchanged. An admitted call returns the exact stored immutable string
        // already frozen at index 8 of that subset — it never calls GetSection or the fresh helper, including on its own
        // error path — so invalid frozen data fails closed with a clear exception, with no live SDK fallback, no
        // synthetic configuration string and no re-freeze. Reuse never enters ReadConfiguredDisplayQuality, so its
        // normal-return 12 CPU-wall diagnostic counts stay fresh-call-only.
        private string MainSnapshotConfiguredQualityBoundSnapshotQa(bool inner)
        {
            int boundGeneration = 0, admissionOpen = 0, stopRequested = 0, qualityGeneration = 0;
            string[] frozenSubset = null;
            if (inner)
            {
                lock (_gateLock)
                {
                    qualityGeneration = _qualityGeneration;
                    boundGeneration = _boundGeneration;
                    admissionOpen = _admissionOpen;
                    stopRequested = _stopRequested;
                    frozenSubset = _frozenQualitySubset;
                }
            }
            if (!inner || qualityGeneration != 1 || boundGeneration <= 0 || admissionOpen != 1 || stopRequested != 0)
            {
                return ReadConfiguredDisplayQuality();
            }

            string subsetLengthDetail = frozenSubset == null ? "<null>" : frozenSubset.Length.ToString(CultureInfo.InvariantCulture);
            if (frozenSubset == null || frozenSubset.Length != RuntimeQualitySubsetNames.Length || frozenSubset.Length < 9)
                throw new InvalidOperationException("NC01_QA_BOUND_QUALITY_SNAPSHOT_SUBSET_INVALID:boundGeneration="
                    + boundGeneration.ToString(CultureInfo.InvariantCulture) + "|subsetLength=" + subsetLengthDetail
                    + "|expectedLength=" + RuntimeQualitySubsetNames.Length.ToString(CultureInfo.InvariantCulture));
            if (!string.Equals(RuntimeQualitySubsetNames[8], "displayQuality", StringComparison.Ordinal))
                throw new InvalidOperationException("NC01_QA_BOUND_QUALITY_SNAPSHOT_FIELD_NAME_UNEXPECTED:boundGeneration="
                    + boundGeneration.ToString(CultureInfo.InvariantCulture) + "|index=8|actual="
                    + TruncateForDiagnostic(RuntimeQualitySubsetNames[8]));
            string frozen = frozenSubset[8];
            if (string.IsNullOrEmpty(frozen))
                throw new InvalidOperationException("NC01_QA_BOUND_QUALITY_SNAPSHOT_TEXT_EMPTY:boundGeneration="
                    + boundGeneration.ToString(CultureInfo.InvariantCulture));
            return frozen;
        }
#endif

        private string ReadActualRenderServiceFlags()
        {
            object renderService = ReflectRequireField(_mainVp, "_renderService");
            if (renderService == null) throw new InvalidOperationException("NC01_ACTUAL_RENDER_SERVICE_NULL");
            return ReadRequiredGetterSnapshot(renderService, ActualRenderFlagNames, "RENDER_FLAG");
        }

        private static string ReadRequiredGetterSnapshot(object target, string[] names, string context)
        {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < names.Length; i++)
            {
                object value;
                try { value = ReflectRequireGetter(target, names[i]); }
                catch (Exception ex) { throw new InvalidOperationException("NC01_" + context + "_GETTER_FAILED:" + names[i], ex); }
                if (value == null) throw new InvalidOperationException("NC01_" + context + "_GETTER_NULL:" + names[i]);
                sb.Append(names[i]).Append('=').Append(FormatInvariant(value)).Append(';');
            }
            return sb.ToString();
        }

        private static string ReadRenderStages(IWindow3D window)
        {
            string snapshot = ReadRenderStagesOrNullWhenEmpty(window);
            if (snapshot == null) throw new InvalidOperationException("NC01_RENDER_STAGES_EMPTY");
            return snapshot;
        }

        private static string ReadRenderStagesOrNullWhenEmpty(IWindow3D window)
        {
            IEnumerable stages;
            try { stages = window.RenderStages as IEnumerable; }
            catch (Exception ex) { throw new InvalidOperationException("NC01_RENDER_STAGES_READ_FAILED", ex); }
            if (stages == null) throw new InvalidOperationException("NC01_RENDER_STAGES_NULL");
            StringBuilder sb = new StringBuilder();
            System.Collections.Generic.List<IntPtr> seenPointers = new System.Collections.Generic.List<IntPtr>();
            int ordinal = 0;
            // Pointer identity is only compared within the same window's lifetime; never across windows or hosting restarts.
            foreach (VisualComponents.Create3D.IRenderStage stage in stages)
            {
                if (stage == null) throw new InvalidOperationException("NC01_RENDER_STAGE_NULL");
                IntPtr pointer;
                string name;
                bool enabled;
                try { pointer = stage.Pointer; name = stage.Name; enabled = stage.IsEnabled; }
                catch (Exception ex) { throw new InvalidOperationException("NC01_RENDER_STAGE_GETTER_FAILED", ex); }
                if (pointer == IntPtr.Zero) throw new InvalidOperationException("NC01_RENDER_STAGE_POINTER_NULL");
                if (seenPointers.Contains(pointer)) throw new InvalidOperationException("NC01_RENDER_STAGE_POINTER_DUPLICATE:" + pointer.ToInt64().ToString("x16", CultureInfo.InvariantCulture));
                seenPointers.Add(pointer);
                if (name == null) throw new InvalidOperationException("NC01_RENDER_STAGE_NAME_UNKNOWN");
                sb.Append("stage").Append(ordinal.ToString(CultureInfo.InvariantCulture))
                  .Append("|pointerHex=").Append(pointer.ToInt64().ToString("x16", CultureInfo.InvariantCulture))
                  .Append("|nameBase64=").Append(Convert.ToBase64String(Encoding.UTF8.GetBytes(name)))
                  .Append("|enabled=").Append(Bool(enabled)).Append(';');
                ordinal++;
            }
            return ordinal == 0 ? null : sb.ToString();
        }

        private static object ReflectRequireGetter(object target, string name)
        {
            PropertyInfo property = FindProperty(target.GetType(), name);
            if (property == null || !property.CanRead) throw new InvalidOperationException("NC01_MEMBER_NOT_READABLE:" + target.GetType().FullName + "." + name);
            return property.GetValue(target, null);
        }
        private static object ReflectTryGetter(object target, string name)
        {
            PropertyInfo p = target == null ? null : FindProperty(target.GetType(), name);
            return p != null && p.CanRead ? p.GetValue(target, null) : null;
        }
        private static object ReflectRequireField(object target, string name)
        {
#if VRSIM_QA_D9_BEGIN_POLL
            FieldInfo field = FindFieldCached(target.GetType(), name);
            if (field != null) return field.GetValue(target);
            throw new InvalidOperationException("NC01_FIELD_MISSING:" + target.GetType().FullName + "." + name);
#else
            for (Type t = target.GetType(); t != null; t = t.BaseType)
            {
                FieldInfo field = t.GetField(name, Inst);
                if (field != null) return field.GetValue(target);
            }
            throw new InvalidOperationException("NC01_FIELD_MISSING:" + target.GetType().FullName + "." + name);
#endif
        }
        private static PropertyInfo FindProperty(Type type, string name)
        {
#if VRSIM_QA_D9_BEGIN_POLL
            return FindPropertyCached(type, name);
#else
            for (Type t = type; t != null; t = t.BaseType)
            {
                PropertyInfo property = t.GetProperty(name, Inst);
                if (property != null) return property;
            }
            return null;
#endif
        }
        private static double[] ToMat16(Matrix m)
        {
            double[] values = new double[16];
            for (int i = 0; i < 16; i++) values[i] = Convert.ToDouble(m[i], CultureInfo.InvariantCulture);
            return values;
        }
        private static bool MatricesClose(double[] a, double[] b) { for (int i = 0; i < 16; i++) if (!IsClose(a[i], b[i])) return false; return true; }

        // Raw-pair narrow absolute comparison: finite operands only, tolerance = RawProjectionTolerance (1e-6, recorded on the const).
        private static bool RawClose(double a, double b)
        {
            return !double.IsNaN(a) && !double.IsNaN(b) && !double.IsInfinity(a) && !double.IsInfinity(b)
                && Math.Abs(a - b) <= RawProjectionTolerance;
        }

        private static void RequireRawFovY(int eye, double fovYDeg)
        {
            if (double.IsNaN(fovYDeg) || double.IsInfinity(fovYDeg) || fovYDeg <= 0.0 || fovYDeg >= 180.0)
                throw new InvalidOperationException("NC01_RAW_PAIR_FOV_INVALID:eye=" + eye.ToString(CultureInfo.InvariantCulture)
                    + "|value=" + FormatInvariant(fovYDeg) + "|rule=finite-and-0deg-to-180deg-exclusive");
        }

        private void RequireRawSourceAspect(double sourceAspect)
        {
            double expected = (double)TargetWidth / (double)TargetHeight;
            if (double.IsNaN(sourceAspect) || double.IsInfinity(sourceAspect) || sourceAspect <= 0.0
                || Math.Abs(sourceAspect - expected) > RawProjectionTolerance)
                throw new InvalidOperationException("NC01_RAW_PAIR_ASPECT_INVALID:value=" + FormatInvariant(sourceAspect)
                    + "|expected=" + TargetWidth.ToString(CultureInfo.InvariantCulture) + "/" + TargetHeight.ToString(CultureInfo.InvariantCulture)
                    + "|tolerance=" + RawProjectionTolerance.ToString("R", CultureInfo.InvariantCulture));
        }

        private static double[] RequireRawView16Finite(int eye, Matrix rawView)
        {
            double[] values = ToMat16(rawView);
            for (int i = 0; i < 16; i++)
                if (double.IsNaN(values[i]) || double.IsInfinity(values[i]))
                    throw new InvalidOperationException("NC01_RAW_PAIR_VIEW_NOT_FINITE:eye=" + eye.ToString(CultureInfo.InvariantCulture)
                        + "|index=" + i.ToString(CultureInfo.InvariantCulture) + "|value=" + FormatInvariant(values[i]));
            return values;
        }

        private static void EnsureHelperLoaded()
        {
            lock (ModuleLock)
            {
                if (_moduleChecked) return;
                if (Marshal.SizeOf(typeof(NC01SurfaceInfoV1)) != 64) throw new InvalidOperationException("NC01_ABI_SIZE_MISMATCH");
#if VRSIM_NATIVE_QA
                string full = Path.GetFullPath(HelperDllAbsolutePath);
#else
                string assemblyLocation;
                try { assemblyLocation = typeof(NC01HostedSceneOwner).Assembly.Location; }
                catch (Exception ex) { throw new InvalidOperationException("NC01_HELPER_ASSEMBLY_LOCATION_UNREADABLE:originaltype=" + ex.GetType().FullName + "|originalmessage=" + ex.Message); }
                if (string.IsNullOrEmpty(assemblyLocation) || !Path.IsPathRooted(assemblyLocation)) throw new InvalidOperationException("NC01_HELPER_ASSEMBLY_NOT_DISK_BACKED:location=" + FormatInvariant(assemblyLocation));
                string assemblyDir;
                try { assemblyDir = Path.GetDirectoryName(assemblyLocation); }
                catch (Exception ex) { throw new InvalidOperationException("NC01_HELPER_ASSEMBLY_DIRECTORY_UNRESOLVABLE:originaltype=" + ex.GetType().FullName + "|originalmessage=" + ex.Message); }
                if (string.IsNullOrEmpty(assemblyDir)) throw new InvalidOperationException("NC01_HELPER_ASSEMBLY_DIRECTORY_UNRESOLVABLE");
                string full = Path.GetFullPath(Path.Combine(assemblyDir, HelperDllFileName));
#endif
                if (!string.Equals(Path.GetFileName(full), HelperDllFileName, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("NC01_DLL_NAME_MISMATCH");
                if (!File.Exists(full)) throw new FileNotFoundException("NC01_DLL_NOT_FOUND:" + full, full);
                IntPtr existing = GetModuleHandleW(HelperDllFileName);
                if (existing != IntPtr.Zero) { if (!ModulePathMatches(existing, full)) throw new InvalidOperationException("NC01_MODULE_ALIAS_CONFLICT"); }
                else
                {
                    IntPtr loaded = LoadLibraryW(full);
                    if (loaded == IntPtr.Zero) throw new InvalidOperationException("NC01_MODULE_LOAD_FAILED:" + Marshal.GetLastWin32Error().ToString(CultureInfo.InvariantCulture));
                    if (!ModulePathMatches(loaded, full)) { FreeLibrary(loaded); throw new InvalidOperationException("NC01_MODULE_ALIAS_CONFLICT"); }
                }
                _moduleChecked = true;
            }
        }
        private static bool ModulePathMatches(IntPtr module, string intendedFull)
        {
            StringBuilder buffer = new StringBuilder(1024);
            uint length = GetModuleFileNameW(module, buffer, (uint)buffer.Capacity);
            if (length == 0 || length >= (uint)buffer.Capacity) return false;
            try { return string.Equals(Path.GetFullPath(buffer.ToString()), intendedFull, StringComparison.OrdinalIgnoreCase); }
            catch (Exception) { return false; }
        }
        private static byte[] StructToBytes(NC01SurfaceInfoV1 info)
        {
            byte[] bytes = new byte[64];
            GCHandle handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);
            try { Marshal.StructureToPtr(info, handle.AddrOfPinnedObject(), false); } finally { handle.Free(); }
            return bytes;
        }

        private void DisableCaptures(string reason)
        {
            // Non-runtime keeps the original plain sticky write semantics.
            if (!_runtimeBackend) { _captureDisabled = true; _disabledReason = reason; return; }
            // Runtime fault close: the sticky reason, the disabled flag and the EFFECTIVE admission close are written as
            // ONE managed transaction under the gate lock. It never clears an already-registered busy, never Disposes
            // anything and never claims Stop completion — only RequestStop writes stopRequested, so an explicit stop can
            // still confirm closure and wait for registered busy to return to zero.
            lock (_gateLock)
            {
                _captureDisabled = true;
                _disabledReason = reason;
                Volatile.Write(ref _admissionOpen, 0);
            }
        }
        private void VerifyAccess() { if (!_ui.CheckAccess()) throw new InvalidOperationException("NC01_UI_THREAD_REQUIRED"); }
        private static bool IsFinitePositive(double value) { return !double.IsNaN(value) && !double.IsInfinity(value) && value > 0.0; }
        private static bool IsClose(double a, double b) { return !double.IsNaN(a) && !double.IsNaN(b) && Math.Abs(a - b) <= 1e-6; }
        private static string Bool(bool value) { return value ? "1" : "0"; }
        private static void Append(StringBuilder sb, string key, object value) { sb.Append(key).Append('=').Append(FormatInvariant(value)).Append(';'); }
        private static string FormatInvariant(object value)
        {
            if (value == null) return "<null>";
            if (value is bool) return Bool((bool)value);
            IFormattable formattable = value as IFormattable;
            return formattable != null ? formattable.ToString(null, CultureInfo.InvariantCulture) : value.ToString();
        }
    }
}
