// VCFrameProducer.cs — PRODUCT-02BC: real stereo + live head tracking.
//
// H2: recording/render ownership lives ENTIRELY in NativeCaptureOwner — constructed and
// Begun on the UI dispatcher before the timer starts; the producer is never a second
// owner. Per LIVE frame: ONE owner.CaptureStereo(rawL, fovL, rawR, fovR) → exact private
// BGRX bytes → ForceAlpha. Static calibration/entry preview keeps owner.Capture(rawView,
// fovY). No manual SetRecordingMode, no hash polling, no 2 s Sleep fallback.
//
// 02BC adds (on top, zero changes to the OpenXR lifecycle):
//  - LATEST_POSE_WINS: each tick reads the freshest runtime eye poses (PHASE 12).
//  - VR_ENTRY_ORIGIN: head delta relative to entry head pose, composed onto the
//    entry desktop camera (PHASE 6/7), CARD16-mapped quantities only (PHASE 8).
//  - Real per-eye cameras from view[0]/view[1] — no hardcoded IPD (PHASE 9/10).
//  - LAST_KNOWN_GOOD on invalid tracking (PHASE 20); restore in finally (PHASE 21).
//
// Camera math (all quantities already CARD16-mapped to SIM mm by the native side):
//   R_M(t) = R_head(t)·R_head0^T          p_M(t) = head(t) − R_M·head0
//   V(t)   = (R_M·R_entry, p_M + R_M·P_entry)      [virtual head = entry camera]
//   eye e: R_cam(e) = R_V·R_head(t)^T·R_eyeSim(e,t)
//          P_cam(e) = P_V + R_V·(R_head(t)^T·(P_eyeSim(e,t) − head(t)))
//   At t=0 this reduces: both cameras = entry camera ∓ IPD/2·(camera right).
//
// 02E ASYNC PIPELINE (this file): the per-eye XR reprojection runs on ONE
// StereoResampleWorker thread per Start generation. UI tick: poll ≤1 completed result →
// submit it with the RESULT's poseSerial/frameSerial (evidence metadata is looked up by
// frameSerial — never re-bound to a fresher pose) → capture fresh L/R → enqueue the
// CLONED job. Worker is pure CPU only; every SDK/native/UI/IO call stays on the UI tick.
// Capture-sequence serial (_captureSerial) is separate from FramesSubmitted, which counts
// ONLY actual successful submits (live rc==0 plus static-calibration rc==0). Honest
// completion telemetry goes to async-pipeline-10s.json; the old synchronous
// performance-stages-10s.json is NO LONGER emitted for the async capture path.
//
// CONVENTION (2026-10-02 pixel experiment on the real SDK): cam.ViewMatrix is
// WORLD-TO-CAMERA (LookAt P=(-200,-1600,-2500); inverse(raw) P=(200,-2500,1600) =
// true eye position; residual 0 vs worldFromEye). All internal R/P arrays above are
// CAMERA-TO-WORLD. Only the boundary helpers convert: RotPosToMat(camera-to-world)
// returns m.Inverse() for a ViewMatrix write; MatToRotPos(raw ViewMatrix) inverts
// before extraction. Saved RAW ViewMatrix values (e.g. _entryView) are restore
// targets — written back directly, NEVER inverted.
// H2-GPU OPT-IN: process env VRSIM_PROJECTION_PIPELINE with the EXACT string "gpu"
// selects the raw-GPU producer; every other value or unset keeps the CPU worker
// resample DEFAULT unchanged. GPU mode keeps the same capture timer/owner/generation,
// calibration, tracking transform and SDK restore/retry, but creates NO
// StereoResampleWorker: each locked tick submits the symmetric-source pair directly via
// VRSIMPlugin.SubmitRawStereo (host validates; native projects and commits on the XR
// side). No ForceAlpha and no CPU resampling run in GPU mode (the shader forces
// alpha=1); capturePairMs is the ACTUAL owner-pair duration only. rc==0 means
// ACCEPTED/ENQUEUED ONLY - never a displayed/headset-pixel claim; the independent
// runtime evidence is the 120-byte native-stats delta in gpu-pipeline-10s.json, which is
// NEVER grafted into async-pipeline-10s.json. Honest limits: capture readback is still
// CPU; the monitor projected preview is pending.
//
// HOSTED-D3D9 OPT-IN (reviewed03): the EXACT env VRSIM_CAPTURE_BACKEND="hosted-d3d9" routes
// Start through NC04.NC01HostedSceneOwner (managed runtime owner) instead of the legacy
// NativeCaptureOwner construction. It is accepted ONLY together with the GPU pipeline, the
// native-1024x1024 capture profile and RawGpuApiSupported; incompatible configurations are
// refused BEFORE hosting — NO fallback. A canceled/failed hosted Start THROWS (never a
// silent return); the whole hosted route keeps _nativeOwner/_worker/_lastWorker null.
// Ready/Capture/Stop behavior belongs to later cards; intermediate builds must NOT run.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Windows.Threading;
using VisualComponents.Create3D;
using NC04 = NC02XrQA;

namespace VRSIM
{
    internal sealed class VCFrameProducer
    {
        internal static int FrameW { get { return CaptureProfile.Width; } }
        internal static int FrameH { get { return CaptureProfile.Height; } }

        private readonly VRSIMPlugin _host;
        private readonly Dispatcher _disp;
        private DispatcherTimer _timer;
        private bool _running, _entryCaptured, _evidencePairSaved;
        private bool _tickInProgress;   // whole-tick reentry guard — SDK CaptureStereo may pump UI callbacks
        private NativeCaptureOwner _nativeOwner;   // H2: SOLE recording/renderer owner
        private IApplication _app;
        private VisualComponents.Create3D.IWindow3D _win;
        private ICamera _cam;
        private Matrix _entryView;
        private double _entryFovY;
        private double _entryGaze = double.NaN;
        private string _evidenceDir;

        // Controlled tick-interval experiment: nominal DispatcherTimer interval resolved ONCE at
        // producer construction from process env VRSIM_CAPTURE_TICK_MS — ONLY the exact string "8"
        // selects 8 ms; any other value or unset keeps the 33 ms default. Experiment only — NO
        // performance claim; real OpenXR 8 ms QA required before promotion.
        private readonly double _nominalTickMs;
        private readonly bool _tickMsEnvSelected;

        // ---------------- 02E async pipeline state (UI-thread only unless noted) ----------------
        private StereoResampleWorker _worker;             // exactly ONE worker — current Start generation
        private StereoResampleWorker _unjoinedWorker;     // retained after a stop timeout; Start refuses until it exits
        private int _generation;                          // bumped by Start (new worker) and by Stop (invalidate first)
        private ulong _captureSerial;                     // per-session native-capture count (counter only — NOT the wire serial)
        private ulong _frameSerial;                       // wire serial for jobs/submits — SAME counter as the static calibration preview, monotonic across both paths
        private ulong _framesSubmitted;                   // ACTUAL successful submits: live rc==0 + static rc==0
        private ulong _liveSubmitted;                     // live completion submits with rc==0 ONLY — static preview excluded (stats card)
        private ulong _lastCompletedFrameSerial;          // monotonic guard on polled completions
        private int _workerFailures, _metaMissing, _metaMismatch, _metaEvicted, _staleDropped, _submitFailures, _jobRejectLogCount;
        private string _stopStatus = "never_stopped";

        // entry origin (PHASE 6): first valid tracking sample after Enter VR
        private bool _originCaptured;
        private EntryCalibration _cal;
        private string _presenceSignal = "NONE";
        private double[] _head0 = new double[3];       // head center at entry (SIM mm)
        private double[] _qHead0 = new double[4];      // head orientation at entry (SIM world←head)
        private double[] _R_E = new double[9];         // entry camera rotation (world←cam)
        private double[] _P_E = new double[3];         // entry camera position (SIM mm)

        // live evidence counters (PHASE 14/15)
        private ulong _poseSerialUsed;
        private int _cameraChangeCount, _invalidTrackingFrames, _renderAttempts, _renderSuccess;
        private double _lastYawDelta, _lastPitchDelta, _pairMs, _renderMs, _copyMs;
        private double _lastStereoPairMs = double.NaN;   // last successful paired capture+alpha ms; NaN = none yet
        private double[] _lastCamL = new double[3];
        private bool _haveLastCam;
        // 02D per-eye asymmetric-FOV reprojection (2026-10-02)
        private ulong _fovInvalidSkips;
        private string _lastFovSkipReason = "";
        private bool _projectionEvidenceWritten;
        private int _freshnessUnconfirmed;
        // The pre-calibration entry preview is STATIC by design: captured ONCE and resubmitted
        // every tick, so the 1.5 s calibration window is never blocked by per-tick native readbacks.
        private byte[] _preCalibrationFrame;
        private double[] _poseAPos; private string _poseAHash;
        private byte[] _poseALeft;
        private bool _poseBSaved;


        // async-pipeline-10s.json window state (successful live submits only)
        private static readonly long AsyncWindowQpc = Stopwatch.Frequency * 10;
        private long _asyncWindowStartQpc, _asyncLastSubmitQpc;
        private int _asyncContentCount;
        private readonly System.Collections.Generic.List<double> _asyncIntervals = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _asyncCapLs = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _asyncCapRs = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _asyncCapPairs = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _asyncRsLs = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _asyncRsRs = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _asyncCapToSubmits = new System.Collections.Generic.List<double>();
        // native-stats delta anchor (seeded at the first successful LIVE submit of each window;
        // never emitted as absolutes — regression/session reset → unknown + re-anchor)
        private bool _asyncBaseValid;
        private long _asyncBaseD3dUploads, _asyncBasePoseMismatch;
        // true → the current window started at its FIRST successful live submit (the seed);
        // that seed frame is excluded from the window FPS. Boundary-anchored windows are false.
        private bool _asyncSeedFrameExcluded;
        private bool _asyncFinalWritten;                     // final flush runs at most once per Start generation
        private StereoResampleWorker _lastWorker;            // stats source after _worker is nulled at stop

        // ---------------- H2 raw-GPU opt-in state (UI-thread only; selection is FROZEN at construction) ----------------
        // Exact env VRSIM_PROJECTION_PIPELINE="gpu" ONLY; every other value or unset = CPU worker resample DEFAULT.
        private readonly bool _gpuPipeline;
        // gpu-pipeline-10s.json window state (successful raw-enqueue acceptances only;
        // NEVER mixed into the async-pipeline-10s.json CPU writer)
        private long _gpuWindowStartQpc, _gpuLastEnqueueQpc;
        private int _gpuEnqueueCount;
        private bool _gpuSeedFrameExcluded, _gpuBaseValid, _gpuFinalWritten;
        // native 120-byte stats delta anchor (gpuPairCommitted / uniqueLiveLayerOk /
        // gpuPairFailed / exactPoseRejected; unavailable/regressed -> null + reanchor)
        private long _gpuBaseCommitted, _gpuBaseLayerOk, _gpuBasePairFailed, _gpuBasePoseRejected;
        private ulong _gpuBaseEpoch;
        private readonly System.Collections.Generic.List<double> _gpuCapPairs = new System.Collections.Generic.List<double>();
        private readonly System.Collections.Generic.List<double> _gpuCapToEnqueues = new System.Collections.Generic.List<double>();
        private ulong _gpuEpochSkips;
        private int _gpuEpochSkipLogCount, _gpuSubmitLogCount, _postCaptureDropLogCount;
        // Hosted LIVE GPU capture profiling breakdown (accepted grc==0 pairs only): reuses the owner's
        // existing NC01CaptureSampleV1 managed timings. CPU wall-clock/blocked durations, NOT GPU
        // dispatch duration; never mixed into any other writer; bounded, per-window, no extra IO.
        private readonly System.Collections.Generic.List<HostedPairTiming> _gpuHostedTimings =
            new System.Collections.Generic.List<HostedPairTiming>();
        private long _gpuHostedTimingMissingOrInvalid;

        // Pure-numeric copy of ONE accepted hosted pair's existing owner timings. Holds no
        // samples/Bgra/owner/SDK objects/delegates; originals are dropped after the copy.
        private struct HostedPairTiming
        {
            public int OriginalGeneration;
            public double PairPrepareMs, PairFinalizeMs;           // shared per pair — recorded once, never eye-summed
            public double EyeCameraSumMs, EyeLockWaitSumMs, EyeSurfaceRenderSumMs;
            public double EyeStageValidationSumMs, EyeAllocateSumMs;
            public double EyeReadbackCallSumMs, EyeUnlockSumMs;    // left+right sums
            public double NativeReadbackSumMs, NativeCopySumMs;    // subintervals of EyeReadbackCallSumMs — never re-added
            public double CapturePairMs;                           // the SAME tick's existing capturePairMs, stamped at collection
        }

        // ---------------- hosted-d3d9 runtime backend (UI-thread only; selection FROZEN at construction) ----------------
        // EXACT env VRSIM_CAPTURE_BACKEND="hosted-d3d9" ONLY; every other value or unset keeps the
        // legacy default route. The hosted route never falls back and never creates a
        // NativeCaptureOwner or StereoResampleWorker for this generation.
        private readonly bool _hostedBackendSelected;
        private NC04.NC01HostedSceneOwner _hostedOwner;
        private int _hostedStartGeneration = -1;
        // readiness timeout/failure state (managed only). The 15 s QPC readiness timeout is
        // evaluated ONLY when the dispatcher timer gets an execution slot — never claimed as a
        // hard deadline against a blocking SDK call.
        private const long HostedReadyTimeoutMs = 15000;
        private long _hostedAwaitSinceQpc;
        private int _hostedAwaitGeneration = -1;
        private int _hostedFailureGeneration = -1;
        private string _hostedFailureReason = string.Empty;
        private bool _hostedFailureNotifyAllowed;
        // Hosted STATIC RAW entry-preview caches (pre-calibration only): independent private CLONES of
        // ONE CaptureRawPair(_entryView, _entryView, …) call, alpha-normalized AFTER cloning — the
        // owner's raw return buffers are never mutated. Both eyes share the ENTRY pose: this is NOT
        // ray-aligned XR stereo and NOT live content. Stats read only the copied managed values of
        // this pair; the legacy single _preCalibrationFrame path stays fully separate.
        private byte[] _hostedPreviewLeft, _hostedPreviewRight;
        private double _hostedPreviewFovL, _hostedPreviewFovR, _hostedPreviewAspect;
        private int _hostedPreviewExtentW, _hostedPreviewExtentH;
        private bool _hostedPreviewOrtho;
        // Hosted STOP bookkeeping: the owner's OWN BoundGeneration is saved at the first Stop entry so
        // retries reuse it — never the already-incremented producer generation. _hostedStopPending
        // lets the Tick finally attempt the pure managed completion once the entered SDK transaction
        // exits. The owner reference is RETAINED by design until the fresh process ends.
        private int _hostedStopBoundGeneration = -1;
        private bool _hostedStopPending;

        // ---------------- OBS-SHARED producer SOURCE stop (Card3B1 — managed half only) ----------------
        // Both Owner stops (RequestStop/TryCompleteStop) are PURE managed and release NO D9 device,
        // official Window or native resource, so the first Stop entry freezes the ORIGINAL hosted
        // owner/ref/BoundGeneration BEFORE _generation++ and immediately closes the original gate +
        // requests the native metadata-only drain EXACTLY ONCE. The producer NEVER Disposes an
        // official object, never creates a new owner and never calls native XRDestroy here: real
        // context exit/cleanup and xrDestroyInstance belong to the native worker plus the future 3B2
        // host deferred timer driving TryStepObsSharedSourceStop / ConfirmObsSharedNativeDestroyed.
        // Budget per ORIGINAL stop: max 32 finite steps OR 5000 ms monotonic QPC (the native worker's
        // own 128 attempts / 1000 ms deadline is EARLIER — a late success can never be faked).
        // Budget / unknown / quarantine / false-finalize / exception → terminal forever with all
        // refs, packets and the combined error retained; Start's retained-ticket refusal keeps Enter
        // blocked and the host must stop automatic retries. Never cleared to fake a clean receipt.
        private NC04.NC01HostedSceneOwner _obsSharedStopOwner;      // frozen original owner reference
        private int _obsSharedStopBoundGeneration = -1;             // frozen POSITIVE BoundGeneration at first Stop entry
        private ulong _obsSharedStopBoundEpoch;                     // original shared epoch from RETAINED tickets, else actual Stop32 — never a fresh current-stats epoch
        private bool _obsSharedStopEntered;                         // ONE-shot same-stop marker (reset only by a fresh allowed Start)
        private bool _obsSharedDrainAttempted;
        private int _obsSharedDrainRequestRc = int.MinValue;        // ACTUAL RequestSharedGpuDrain rc (int.MinValue = exception / never attempted)
        private Exception _obsSharedStopError;                      // combined actual rc/exception evidence, retained; never overwrites the 3A LastObsSharedCaptureFault
        private bool _obsSharedFinallyActive;                       // TRUE across the WHOLE Tick finally (covers the window after _tickInProgress=false)
        private int _obsSharedSourceStopSteps;
        private long _obsSharedSourceStopBeginQpc;
        // Recorded ONCE at the first Stop/capture-failure freeze — the 5000 ms monotonic deadline
        // anchor. Evaluated BEFORE the tick/finally/ownerBusy early returns so a busy owner can never
        // keep resetting the budget; expiry only marks the state terminal, it never promises to
        // interrupt a blocking SDK/GPU call.
        private long _obsSharedFirstStopQpc;
        private bool _obsSharedStopTerminal;
        private bool _obsSharedSourceReady;                         // SOURCE finalized (real rc0-verified OR proven never-bound)
        private bool _obsSharedWholeStopVerified;                   // real destroy rc0 + native OFF + original TryCompleteStop true
        private bool _obsSharedNeverBoundSourceReady;               // DISTINCT never-bound proof — never conflated with a real SourceUIFinalize rc==0
        private VRSIMPlugin.SharedGpuStopStateV1 _obsSharedFinalStopState;
        private bool _obsSharedFinalStopStateValid;                 // stored ONLY from a real post-finalize ReadSharedGpuStop match
        // Frozen SharedGpuStopStateV1 flag bits. MUST equal the native wire definition (reviewed
        // against the native 32-byte stop metadata): bit0 bridge CLOSED, bit1 WORKER_DRAINED,
        // bit2 SOURCE_UI finalized; the verified final readback is EXACTLY 0b111 with pending 0 and
        // zero unknown high bits. A mismatch at any point is an honest terminal, never an inferred 7.
        private const uint ObsSharedStopFlagsBridgeClosedV1 = 1u;
        private const uint ObsSharedStopFlagsWorkerDrainedV1 = 2u;
        private const uint ObsSharedStopFlagsSourceFinalizedV1 = 7u;

        // ---------------- hosted CAPTURE SCHEDULER opt-in (UI-thread only; selection FROZEN at construction) ----------------
        // EXACT env VRSIM_CAPTURE_SCHEDULER="coalesced-background" ONLY, and only effective together
        // with the frozen hosted-d3d9 backend AND gpu pipeline — never standalone. ONE retained
        // Background DispatcherOperation chains captures; NOT a background SDK, NOT a 0 ms
        // DispatcherTimer, NOT Thread.Sleep, NOT busy-wait/SpinWait, NOT multithreaded capture.
        // Timer/nominal-pulse timings are scheduling evidence ONLY — never an FPS claim.
        private readonly bool _coalescedBackgroundScheduler;
        private HostedPulseToken _coalescedPulseToken;   // at most ONE pending/executing pulse per instance
        private bool _coalescedPulseOrigin;              // Tick-entry marker: this Tick runs from the retained pulse
        // ---------------- kernel-pulse capture scheduler opt-in (isolated; VRSIM_CAPTURE_SCHEDULER="kernel-pulse") ----------------
        // The TESTED KernelUiPulse drives UI-thread Ticks on the frozen NC02 hosted route ONLY.
        // At most ONE scheduler source: kernel-pulse OR coalesced OR timer, never mixed. Stop is
        // ASYNC-ONLY: pending/unverified stops THROW before any StopHostedCore/SDK cleanup and all
        // pulse/task/owner refs stay RETAINED for the host exact-source 100 ms retry. Scheduler
        // labels are scheduling facts only — never an FPS or VR-acceleration claim.
        private readonly bool _kernelPulseRequested;
        // Kernel-render-120 scheduler opt-in (bounded shared high-res experiment): EXACT
        // VRSIM_CAPTURE_SCHEDULER="kernel-render-120" ONLY, accepted in Start ONLY together with
        // obs-shared + hosted-d3d9 + gpu + native-1536x1536 — otherwise it THROWS before any
        // owner/SDK work; never silently ignored, no fallback. Same single KernelUiPulse closure;
        // renderPaced=true selects Render priority + max 120 UI-action starts/second. Scheduler
        // labels/facts only — never an FPS claim.
        private readonly bool _kernelRender120Requested;
        private KernelUiPulse _kernelPulse;
        private System.Threading.Tasks.Task<KernelUiPulseStopResult> _kernelPulseStopTask;
        private KernelUiPulseStopResult _kernelPulseStopResult;
        private string _kernelPulseStopError;
        // ---------------- OBS-SHARED kernel-render-120 scheduler STOP integration ----------------
        // The exact pulse + exact stop task frozen inside the SAME first shared Stop/failure call
        // are RETAINED for the WHOLE shared cleanup (never cleared before WholeStop, never
        // substituted) and the pure-managed quiet helper verifies the observer-published immutable
        // result against THESE frozen refs. everCreated distinguishes an honest neverCreated
        // (canceled strictly BEFORE any pulse creation, every kernel field absent) from a lost
        // proof — a successful stop result is never synthesized. Plain shared timer needs no pulse.
        private bool _kernelRender120PulseEverCreated;
        private KernelUiPulse _obsSharedStopPulse;
        private System.Threading.Tasks.Task<KernelUiPulseStopResult> _obsSharedStopPulseTask;
        // Hosted failure-notification bookkeeping: at most one Dispatcher queue per failure
        // generation (_hostedFailureNotifyQueuedGeneration) and at most one host report per
        // generation (_hostedFailureNotifySentGeneration). Both reset naturally per new gen.
        private int _hostedFailureNotifyQueuedGeneration = -1;
        private int _hostedFailureNotifySentGeneration = -1;

        // bounded evidence metadata: frameSerial -> immutable capture row (P_Lcam clone, poseSerial,
        // capL/R ms, capture-start QPC). Max 8 rows, oldest evicted beyond the bound.
        private const int MaxCaptureMetaRows = 8;
        private readonly System.Collections.Generic.Dictionary<ulong, CaptureMetaRow> _captureMeta =
            new System.Collections.Generic.Dictionary<ulong, CaptureMetaRow>();
        private readonly System.Collections.Generic.List<ulong> _captureMetaOrder =
            new System.Collections.Generic.List<ulong>();

        // ---------------- NC02-XR staging (P1 — UNWIRED managed state + immutable metadata) ----------------
        // EXACT env VRSIM_READBACK_PIPELINE="nc02-stage" ONLY; every other value or unset keeps the
        // current pipeline. NO staging API is called in P1; later cards consume these fields.
        // The managed pending limit is FROZEN at 1 (the native helper's own 2 slots are unchanged).
        private readonly bool _nc02StagingRequested;
        private readonly int _nc02ManagedPendingLimit = 1;

        // ---------------- OBS-SHARED typed GPU capture (Card3A — compiled hooks; runtime activation OFF) ----------------
        // EXACT constructor-local readbackEnv == "obs-shared" ONLY (never reread in Start/Tick/callback).
        // Shared rides hosted-d3d9 + gpu + native-1536x1536 + the plain DispatcherTimer; every other mode
        // is unchanged. The hard gate below keeps runtime activation DISABLED until the Native2B worker/
        // shutdown and Managed3B stop lifecycle cards pass independent review/compile; this card wires
        // compilable PREPARE→COMMIT→DISCARD hooks only.
        private readonly bool _obsSharedRequested;
        private const bool ManagedObsSharedActivation = true;
        // Frozen shared-wire numbers (SharedStereoWire.h) mirrored producer-side; the VRSIMPlugin wire
        // constants are private, so the identity/ticket shape checks use these same frozen values directly.
        private const uint ObsSharedWireVersionV1 = 1u;
        private const uint ObsSharedTicketBytesV1 = 64u;
        private const uint ObsSharedIdentityBytesV1 = 192u;
#if VRSIM_SHARED_2048
        private const uint ObsSharedTicketEdgeV1 = 2048u;
        private const string ObsSharedCaptureProfileV1 = "native-2048x2048";
#else
        private const uint ObsSharedTicketEdgeV1 = 1536u;
        private const string ObsSharedCaptureProfileV1 = "native-1536x1536";
#endif
        private const uint ObsSharedTicketFormat88V1 = 88u;
        private const uint ObsSharedSlotCountV1 = 3u;
        // Shared bridge slot-state FREE value (SharedStereoWire.h slot state). A COMMITTED packet may
        // be RECYCLED for a new managed ledger entry ONLY when the native bridge reports its slotIndex
        // genuinely FREE — never because COMMIT returned 0, a runtime frame advanced, or a new
        // generation started. MUST equal the frozen native FREE state; compile/review verifies it.
        private const uint ObsSharedBridgeSlotStateFreeV1 = 0u;
        // Bounded three-entry ledger: preallocated PREPARED ticket + original PREPARE identity + a
        // locally-owned/discard-pending flag. The MANAGED ledger index need NOT equal native slotIndex;
        // a free entry is chosen before owner work so an unexpectedly malformed ticket is preserved
        // without indexing an invalid native slot or losing its exact contents. NEVER cleared to fake
        // a clean receipt — unresolved entries block Start and are handed to the Managed3B stop card.
        private readonly VRSIMPlugin.SharedGpuTicketV1[] _obsSharedTickets =
            new VRSIMPlugin.SharedGpuTicketV1[3];
        private readonly VRSIMPlugin.SharedGpuCaptureIdentityV1[] _obsSharedIdentities =
            new VRSIMPlugin.SharedGpuCaptureIdentityV1[3];
        private readonly bool[] _obsSharedOwned = new bool[3];
        // TRUE once a COMMIT returns native Completed: the FULL ticket + original identity stay RETAINED
        // (not dropped) so the Managed3B stop card can still exact-Discard a COMMITTED-but-unread packet.
        // owned=false + committed=true is the "COMMIT accepted, native owns the ring slot, managed keeps
        // the packet" state. An entry becomes reusable ONLY after a proven FREE native slot (see ledger
        // release below) — never merely because the COMMIT succeeded or a newer generation started.
        private readonly bool[] _obsSharedCommitted = new bool[3];
        // Honest counters: PREPARE-granted / COMMIT-accepted / rejected-fault ONLY. A native COMMIT 0 is
        // NOT a displayed frame and never inflates content FPS; unique real content stays the native
        // distinct-frame xrEndFrame counter.
        private ulong _obsSharedPrepared, _obsSharedCommitAccepted, _obsSharedRejected;
        // Normal shared backpressure: all three managed ledger entries still hold an unresolved
        // (owned/committed) packet, so THIS tick simply skips — a scheduling/skipped fact ONLY, never a
        // fault, never a ticket release, never a fallback or a retry, and never an FPS claim.
        private ulong _obsSharedBackpressureSkips;
        // READ-ONLY QA evidence: the last combined OBS-SHARED fault (owner/native body exception
        // including the Unlock AggregateException with its inner exceptions, merged with any DISCARD
        // failure). Holds no pixels, no SDK object and writes no file — exposed only via the getter.
        private Exception _obsSharedLastFault;
        /// <summary>READ-ONLY QA: the last retained combined OBS-SHARED capture fault evidence, or null.
        /// Never clears the retained ledger packet — the Managed3B stop card still owns exact cleanup.</summary>
        internal Exception LastObsSharedCaptureFault { get { return _obsSharedLastFault; } }
        /// <summary>READ-ONLY QA: pure numeric shared backpressure skipped-tick count (no IO, no claim).</summary>
        internal ulong ObsSharedBackpressureSkips { get { return _obsSharedBackpressureSkips; } }

        private readonly System.Collections.Generic.Dictionary<ulong, Nc02XrCaptureMetaRow> _nc02CaptureRows =
            new System.Collections.Generic.Dictionary<ulong, Nc02XrCaptureMetaRow>(2);
        // bounded poll maps, preallocated: index 0/1 pair a native pair id with its frame serial.
        private readonly ulong[] _nc02MapPairIds = new ulong[2];
        private readonly ulong[] _nc02MapFrameSerials = new ulong[2];
        private readonly int[] _nc02PollCounts = new int[2];
        // stop-drain bookkeeping (pure managed; later card)
        private long _nc02DrainBeginQpc;
        private int _nc02DrainAttempts;
        private bool _nc02DrainBudgetExpired;
        // pure-number monitoring counters (no per-frame lists, no BGRA, no QA framework, no IO)
        private ulong _nc02SubmitAccepted, _nc02Collected, _nc02NativeEnqueueAccepted, _nc02PendingPolls, _nc02Dropped;
        private double _nc02LastSourceLatencyMs = double.NaN;   // NaN = unknown yet; never 0-as-unknown
        private double _nc02LastPoseToEnqueueMs = double.NaN;
        private double _nc02LastRawMatrixMaxAbsDelta = double.NaN;
        private double _nc02MaxRawMatrixAbsDelta = double.NaN;

        // ------- NC02 cost diagnostics: SOURCE_PAIR_COLLECTED_NOT_ENQUEUE_OR_DISPLAY -------
        // Bounded numeric-only last/sum state for root readonly QA. The lock protects ONLY managed
        // update / reset / snapshot copy — NEVER an SDK, native, callback or IO call. The snapshot
        // Dictionary is a fresh copy of values; no result, BGRA, raw176 byte[], owner, SDK object or
        // delegate is retained here. Unknown last values report NaN (ms) / null (identities, ticks,
        // QPC, counts); double sums are NaN until the first valid commit; integer sums are
        // identified by validCommitCount. Host elapsed/blocked windows only — never GPU durations.
        private readonly object _nc02DiagLock = new object();
        private int _nc02DiagGeneration = -1;
        private bool _nc02DiagLastKnown;
        private ulong _nc02DiagLastPairId, _nc02DiagLastFrameSerial, _nc02DiagLastPoseSerial, _nc02DiagLastNativeXrEpoch;
        private double _nc02DiagLastSubmitNativeMs = double.NaN, _nc02DiagLastCollectNativeMs = double.NaN,
            _nc02DiagLastEyeLockMs = double.NaN, _nc02DiagLastSurfaceRenderMs = double.NaN,
            _nc02DiagLastPairPrepareMs = double.NaN, _nc02DiagLastPairFinalizeMs = double.NaN;
        private double _nc02DiagSumSubmitNativeMs = double.NaN, _nc02DiagSumCollectNativeMs = double.NaN,
            _nc02DiagSumEyeLockMs = double.NaN, _nc02DiagSumSurfaceRenderMs = double.NaN,
            _nc02DiagSumPairPrepareMs = double.NaN, _nc02DiagSumPairFinalizeMs = double.NaN;
        private ulong _nc02DiagLastGetRtdTicks, _nc02DiagLastIssueTicks, _nc02DiagLastQueryTicks,
            _nc02DiagLastLockTicks, _nc02DiagLastRowCopyTicks, _nc02DiagLastUnlockTicks,
            _nc02DiagLastSubmitTotalTicks, _nc02DiagLastPollTotalTicks;
        private ulong _nc02DiagLastNativeSubmitQpc, _nc02DiagLastNativeCollectQpc;
        private long _nc02DiagLastCaptureBeginQpc, _nc02DiagLastCaptureEndQpc;
        private ulong _nc02DiagLastQueryAttempts, _nc02DiagLastLeftLockAttempts, _nc02DiagLastRightLockAttempts;
        private ulong _nc02DiagSumQueryAttempts, _nc02DiagSumLeftLockAttempts, _nc02DiagSumRightLockAttempts;
        private long _nc02DiagObservedCollected, _nc02DiagValidCommits, _nc02DiagInvalidCommits;

        /// <summary>READ-ONLY QA snapshot: a NEW Dictionary of numeric copies in the
        /// SOURCE_PAIR_COLLECTED_NOT_ENQUEUE_OR_DISPLAY scope. Managed copy under the lock only;
        /// no SDK/native query runs here. ms values are host-measured from the result fields
        /// (SubmitNativeMs includes the FIRST Create; CollectNativeMs includes every poll of this
        /// OLD ticket); ticks are the raw THIS-pair native windows preserved without conversion.</summary>
        internal System.Collections.Generic.Dictionary<string, object> Nc02Diagnostics
        {
            get
            {
                lock (_nc02DiagLock)
                {
                    bool known = _nc02DiagLastKnown;
                    var d = new System.Collections.Generic.Dictionary<string, object>(48);
                    d["scope"] = "SOURCE_PAIR_COLLECTED_NOT_ENQUEUE_OR_DISPLAY";
                    d["qpcFrequency"] = (object)System.Diagnostics.Stopwatch.Frequency;
                    d["generation"] = _nc02DiagGeneration >= 0 ? (object)_nc02DiagGeneration : null;
                    d["observedCollectedCount"] = (object)_nc02DiagObservedCollected;
                    d["validCommitCount"] = (object)_nc02DiagValidCommits;
                    d["invalidCommitCount"] = (object)_nc02DiagInvalidCommits;
                    d["lastPairId"] = known ? (object)_nc02DiagLastPairId : null;
                    d["lastFrameSerial"] = known ? (object)_nc02DiagLastFrameSerial : null;
                    d["lastPoseSerial"] = known ? (object)_nc02DiagLastPoseSerial : null;
                    d["lastNativeXrEpoch"] = known ? (object)_nc02DiagLastNativeXrEpoch : null;
                    // Unknown / non-finite ms report actual null so ordinary JSON serialization stays
                    // valid; committed finite measurements (including a real measured zero) are unchanged.
                    d["lastSubmitNativeMs"] = known ? (object)_nc02DiagLastSubmitNativeMs : null;
                    d["lastCollectNativeMs"] = known ? (object)_nc02DiagLastCollectNativeMs : null;
                    d["lastEyeLockMs"] = known ? (object)_nc02DiagLastEyeLockMs : null;
                    d["lastSurfaceRenderMs"] = known ? (object)_nc02DiagLastSurfaceRenderMs : null;
                    d["lastPairPrepareMs"] = known ? (object)_nc02DiagLastPairPrepareMs : null;
                    d["lastPairFinalizeMs"] = known ? (object)_nc02DiagLastPairFinalizeMs : null;
                    d["sumSubmitNativeMs"] = double.IsNaN(_nc02DiagSumSubmitNativeMs) || double.IsInfinity(_nc02DiagSumSubmitNativeMs) ? null : (object)_nc02DiagSumSubmitNativeMs;
                    d["sumCollectNativeMs"] = double.IsNaN(_nc02DiagSumCollectNativeMs) || double.IsInfinity(_nc02DiagSumCollectNativeMs) ? null : (object)_nc02DiagSumCollectNativeMs;
                    d["sumEyeLockMs"] = double.IsNaN(_nc02DiagSumEyeLockMs) || double.IsInfinity(_nc02DiagSumEyeLockMs) ? null : (object)_nc02DiagSumEyeLockMs;
                    d["sumSurfaceRenderMs"] = double.IsNaN(_nc02DiagSumSurfaceRenderMs) || double.IsInfinity(_nc02DiagSumSurfaceRenderMs) ? null : (object)_nc02DiagSumSurfaceRenderMs;
                    d["sumPairPrepareMs"] = double.IsNaN(_nc02DiagSumPairPrepareMs) || double.IsInfinity(_nc02DiagSumPairPrepareMs) ? null : (object)_nc02DiagSumPairPrepareMs;
                    d["sumPairFinalizeMs"] = double.IsNaN(_nc02DiagSumPairFinalizeMs) || double.IsInfinity(_nc02DiagSumPairFinalizeMs) ? null : (object)_nc02DiagSumPairFinalizeMs;
                    d["lastGetRenderTargetDataTicks"] = known ? (object)_nc02DiagLastGetRtdTicks : null;
                    d["lastIssueTicks"] = known ? (object)_nc02DiagLastIssueTicks : null;
                    d["lastQueryTicks"] = known ? (object)_nc02DiagLastQueryTicks : null;
                    d["lastLockTicks"] = known ? (object)_nc02DiagLastLockTicks : null;
                    d["lastRowCopyTicks"] = known ? (object)_nc02DiagLastRowCopyTicks : null;
                    d["lastUnlockTicks"] = known ? (object)_nc02DiagLastUnlockTicks : null;
                    d["lastSubmitTotalTicks"] = known ? (object)_nc02DiagLastSubmitTotalTicks : null;
                    d["lastPollTotalTicks"] = known ? (object)_nc02DiagLastPollTotalTicks : null;
                    d["lastQueryAttempts"] = known ? (object)_nc02DiagLastQueryAttempts : null;
                    d["lastLeftLockAttempts"] = known ? (object)_nc02DiagLastLeftLockAttempts : null;
                    d["lastRightLockAttempts"] = known ? (object)_nc02DiagLastRightLockAttempts : null;
                    d["sumQueryAttempts"] = (object)_nc02DiagSumQueryAttempts;
                    d["sumLeftLockAttempts"] = (object)_nc02DiagSumLeftLockAttempts;
                    d["sumRightLockAttempts"] = (object)_nc02DiagSumRightLockAttempts;
                    d["lastCaptureBeginQpc"] = known ? (object)_nc02DiagLastCaptureBeginQpc : null;
                    d["lastCaptureEndQpc"] = known ? (object)_nc02DiagLastCaptureEndQpc : null;
                    d["lastNativeSubmitQpc"] = known ? (object)_nc02DiagLastNativeSubmitQpc : null;
                    d["lastNativeCollectQpc"] = known ? (object)_nc02DiagLastNativeCollectQpc : null;
                    return d;
                }
            }
        }

        // Immutable per-capture metadata row for the NC02 staging pipeline. Holds ONLY readonly
        // numerics, the already-immutable identity and PRIVATE numeric clones taken BEFORE any native
        // Submit. NEVER retains a Matrix/SDK/COM/owner/Func/dispatcher reference or pixel byte[].
        // Both the row type and its dictionary are private to this producer — nothing is exposed.
        private sealed class Nc02XrCaptureMetaRow
        {
            internal readonly NC02XrQA.NC02XrCaptureIdentityV1 Identity;
            internal readonly VRSIMPlugin.RawProjectionPlanV1 LeftPlan, RightPlan;
            internal readonly double LeftFovLeftRad, LeftFovRightRad, LeftFovUpRad, LeftFovDownRad;
            internal readonly double RightFovLeftRad, RightFovRightRad, RightFovUpRad, RightFovDownRad;
            internal readonly float LeftTanL32, LeftTanR32, LeftTanU32, LeftTanD32, LeftTx32, LeftTy32;
            internal readonly float RightTanL32, RightTanR32, RightTanU32, RightTanD32, RightTx32, RightTy32;
            internal readonly double Aspect = 1.0;
            internal readonly bool Perspective = true;
            private readonly double[] _leftEyePosition3 = new double[3];
            private readonly double[] _rightEyePosition3 = new double[3];
            private readonly double[] _leftEyeQuaternion4 = new double[4];
            private readonly double[] _rightEyeQuaternion4 = new double[4];
            internal readonly double SourceFovLeft, SourceFovRight, YawDeg;
            internal readonly int Width, Height, Stride, QualityGeneration;
            // private numeric clones — element-filled once in the ctor, never reassigned afterwards.
            private readonly double[] _rawLeft16 = new double[16];
            private readonly double[] _rawRight16 = new double[16];
            // diagnostic-only result field; readonly Identity/plans/private input clones untouched
            internal double LastRawMatrixMaxAbsDelta = double.NaN;
            private readonly double[] _entryRotation9 = new double[9];
            private readonly double[] _entryPosition3 = new double[3];
            private readonly double[] _anchorPosition3 = new double[3];
            private readonly double[] _anchorQuaternion4 = new double[4];

            internal Nc02XrCaptureMetaRow(
                NC02XrQA.NC02XrCaptureIdentityV1 identity,
                Matrix rawLeft, Matrix rawRight,
                VRSIMPlugin.RawProjectionPlanV1 leftPlan, VRSIMPlugin.RawProjectionPlanV1 rightPlan,
                VRSIMPlugin.VRSIMEyeView leftEye, VRSIMPlugin.VRSIMEyeView rightEye,
                double sourceFovLeft, double sourceFovRight, int width, int height, int stride,
                int qualityGeneration, double yawDeg,
                double[] entryRotation, double[] entryPosition,
                double[] anchorPosition, double[] anchorQuaternion)
            {
                if (identity == null)
                    throw new ArgumentNullException("identity");
                if (identity.FrameSerial == 0UL || identity.PoseSerial == 0UL || identity.NativeXrEpoch == 0UL ||
                    identity.OriginalGeneration <= 0 ||
                    identity.PoseSampleStartQpc <= 0L ||
                    identity.PoseSampleEndQpc < identity.PoseSampleStartQpc ||
                    identity.CaptureBeginQpc < identity.PoseSampleEndQpc)
                    throw new InvalidOperationException("nc02_row_bad_identity");
                if (qualityGeneration != 1)
                    throw new InvalidOperationException("nc02_row_bad_quality_generation");
                if (width < 2 || width > 2048 || height < 2 || height > 2048 ||
                    stride != checked(width * 4) ||
                    checked((long)stride * (long)height) > 32L * 1024L * 1024L)
                    throw new InvalidOperationException("nc02_row_bad_capture_bounds");
                if (sourceFovLeft <= 0.0 || sourceFovLeft >= 180.0 ||
                    sourceFovRight <= 0.0 || sourceFovRight >= 180.0)
                    throw new InvalidOperationException("nc02_row_source_fov_out_of_range");
                if (leftEye.posMm == null || leftEye.quatXyzw == null ||
                    rightEye.posMm == null || rightEye.quatXyzw == null ||
                    leftEye.posMm.Length != 3 || leftEye.quatXyzw.Length != 4 ||
                    rightEye.posMm.Length != 3 || rightEye.quatXyzw.Length != 4)
                    throw new InvalidOperationException("nc02_row_bad_eye_view_shape");
                if (leftPlan.structBytes != 56u || leftPlan.version != 1u ||
                    rightPlan.structBytes != 56u || rightPlan.version != 1u)
                    throw new InvalidOperationException("nc02_row_bad_plan_header");
                double[] planD12 = new double[12];
                planD12[0] = leftPlan.tanL; planD12[1] = leftPlan.tanR; planD12[2] = leftPlan.tanU;
                planD12[3] = leftPlan.tanD; planD12[4] = leftPlan.tx; planD12[5] = leftPlan.ty;
                planD12[6] = rightPlan.tanL; planD12[7] = rightPlan.tanR; planD12[8] = rightPlan.tanU;
                planD12[9] = rightPlan.tanD; planD12[10] = rightPlan.tx; planD12[11] = rightPlan.ty;
                for (int i = 0; i < 12; i++)
                {
                    if (double.IsNaN(planD12[i]) || double.IsInfinity(planD12[i]))
                        throw new InvalidOperationException("nc02_row_plan_not_finite");
                }
                if (!(planD12[0] < 0.0) || !(planD12[1] > 0.0) || !(planD12[2] > 0.0) || !(planD12[3] < 0.0) ||
                    !(planD12[4] > 0.0) || !(planD12[5] > 0.0) ||
                    !(planD12[6] < 0.0) || !(planD12[7] > 0.0) || !(planD12[8] > 0.0) || !(planD12[9] < 0.0) ||
                    !(planD12[10] > 0.0) || !(planD12[11] > 0.0))
                    throw new InvalidOperationException("nc02_row_plan_sign");
                float[] planF12 = new float[12];
                for (int i = 0; i < 12; i++)
                {
                    planF12[i] = (float)planD12[i];
                    if (float.IsNaN(planF12[i]) || float.IsInfinity(planF12[i]))
                        throw new InvalidOperationException("nc02_row_plan_cast_not_finite");
                }
                if (!(planF12[4] > 0.0f) || !(planF12[5] > 0.0f) ||
                    !(planF12[10] > 0.0f) || !(planF12[11] > 0.0f))
                    throw new InvalidOperationException("nc02_row_plan_cast_sign");
                double[] fovD8 = new double[8];
                fovD8[0] = leftEye.fovLeft; fovD8[1] = leftEye.fovRight;
                fovD8[2] = leftEye.fovUp; fovD8[3] = leftEye.fovDown;
                fovD8[4] = rightEye.fovLeft; fovD8[5] = rightEye.fovRight;
                fovD8[6] = rightEye.fovUp; fovD8[7] = rightEye.fovDown;
                for (int i = 0; i < 8; i++)
                {
                    if (double.IsNaN(fovD8[i]) || double.IsInfinity(fovD8[i]) ||
                        !(fovD8[i] > -Math.PI / 2.0) || !(fovD8[i] < Math.PI / 2.0))
                        throw new InvalidOperationException("nc02_row_eye_fov_invalid");
                }
                if (!(fovD8[0] < 0.0) || !(fovD8[1] > 0.0) || !(fovD8[2] > 0.0) || !(fovD8[3] < 0.0) ||
                    !(fovD8[4] < 0.0) || !(fovD8[5] > 0.0) || !(fovD8[6] > 0.0) || !(fovD8[7] < 0.0))
                    throw new InvalidOperationException("nc02_row_eye_fov_sign");
                if (entryRotation == null || entryRotation.Length != 9)
                    throw new InvalidOperationException("nc02_row_bad_entry_rotation_shape");
                if (entryPosition == null || entryPosition.Length != 3)
                    throw new InvalidOperationException("nc02_row_bad_entry_position_shape");
                if (anchorPosition == null || anchorPosition.Length != 3)
                    throw new InvalidOperationException("nc02_row_bad_anchor_position_shape");
                if (anchorQuaternion == null || anchorQuaternion.Length != 4)
                    throw new InvalidOperationException("nc02_row_bad_anchor_quaternion_shape");
                if (double.IsNaN(sourceFovLeft) || double.IsInfinity(sourceFovLeft) ||
                    double.IsNaN(sourceFovRight) || double.IsInfinity(sourceFovRight) ||
                    double.IsNaN(yawDeg) || double.IsInfinity(yawDeg))
                    throw new InvalidOperationException("nc02_row_nonfinite_fov_or_yaw");
                if (width <= 0 || height <= 0 || stride <= 0)
                    throw new InvalidOperationException("nc02_row_bad_capture_geometry");

                // Matrix clone in the owner's EXACT ToMat16 order: m[0..15] via the indexer, no
                // assumed row/column layout, no m[row,column] indexing. The Matrix itself is dropped.
                for (int i = 0; i < 16; i++)
                {
                    double v = Convert.ToDouble(rawLeft[i]);
                    if (double.IsNaN(v) || double.IsInfinity(v))
                        throw new InvalidOperationException("nc02_row_raw_left_not_finite");
                    _rawLeft16[i] = v;
                }
                for (int i = 0; i < 16; i++)
                {
                    double v = Convert.ToDouble(rawRight[i]);
                    if (double.IsNaN(v) || double.IsInfinity(v))
                        throw new InvalidOperationException("nc02_row_raw_right_not_finite");
                    _rawRight16[i] = v;
                }
                for (int i = 0; i < 9; i++)
                {
                    double v = entryRotation[i];
                    if (double.IsNaN(v) || double.IsInfinity(v))
                        throw new InvalidOperationException("nc02_row_entry_rotation_not_finite");
                    _entryRotation9[i] = v;
                }
                for (int i = 0; i < 3; i++)
                {
                    double v = entryPosition[i];
                    if (double.IsNaN(v) || double.IsInfinity(v))
                        throw new InvalidOperationException("nc02_row_entry_position_not_finite");
                    _entryPosition3[i] = v;
                }
                for (int i = 0; i < 3; i++)
                {
                    double v = anchorPosition[i];
                    if (double.IsNaN(v) || double.IsInfinity(v))
                        throw new InvalidOperationException("nc02_row_anchor_position_not_finite");
                    _anchorPosition3[i] = v;
                }
                for (int i = 0; i < 4; i++)
                {
                    double v = anchorQuaternion[i];
                    if (double.IsNaN(v) || double.IsInfinity(v))
                        throw new InvalidOperationException("nc02_row_anchor_quaternion_not_finite");
                    _anchorQuaternion4[i] = v;
                }

                for (int i = 0; i < 3; i++)
                {
                    double vl = leftEye.posMm[i];
                    double vr = rightEye.posMm[i];
                    if (double.IsNaN(vl) || double.IsInfinity(vl) ||
                        double.IsNaN(vr) || double.IsInfinity(vr))
                        throw new InvalidOperationException("nc02_row_eye_position_not_finite");
                    _leftEyePosition3[i] = vl;
                    _rightEyePosition3[i] = vr;
                }
                for (int i = 0; i < 4; i++)
                {
                    double ql = leftEye.quatXyzw[i];
                    double qr = rightEye.quatXyzw[i];
                    if (double.IsNaN(ql) || double.IsInfinity(ql) ||
                        double.IsNaN(qr) || double.IsInfinity(qr))
                        throw new InvalidOperationException("nc02_row_eye_quaternion_not_finite");
                    _leftEyeQuaternion4[i] = ql;
                    _rightEyeQuaternion4[i] = qr;
                }

                Identity = identity;
                LeftPlan = leftPlan;
                RightPlan = rightPlan;
                LeftFovLeftRad = leftEye.fovLeft;
                LeftFovRightRad = leftEye.fovRight;
                LeftFovUpRad = leftEye.fovUp;
                LeftFovDownRad = leftEye.fovDown;
                RightFovLeftRad = rightEye.fovLeft;
                RightFovRightRad = rightEye.fovRight;
                RightFovUpRad = rightEye.fovUp;
                RightFovDownRad = rightEye.fovDown;
                LeftTanL32 = planF12[0];
                LeftTanR32 = planF12[1];
                LeftTanU32 = planF12[2];
                LeftTanD32 = planF12[3];
                LeftTx32 = planF12[4];
                LeftTy32 = planF12[5];
                RightTanL32 = planF12[6];
                RightTanR32 = planF12[7];
                RightTanU32 = planF12[8];
                RightTanD32 = planF12[9];
                RightTx32 = planF12[10];
                RightTy32 = planF12[11];
                SourceFovLeft = sourceFovLeft;
                SourceFovRight = sourceFovRight;
                YawDeg = yawDeg;
                Width = width;
                Height = height;
                Stride = stride;
                QualityGeneration = qualityGeneration;
            }

            // Pure managed numeric/array/string match of a completed XR result against this frozen
            // old-ticket row. NO SDK, NO owner, NO Matrix, NO native, NO stats, NO IO, NO logging,
            // NO per-frame hash, NO retention of the completed reference. Returns null on success
            // else a short reason tag. The result itself carries no plan — the row's 56B plan
            // header / finite / sign / float-cast guarantees from the ctor are preserved by
            // reference, never re-derived from a "latest pose / new FOV".
            internal string MatchesCollected(NC02XrQA.NC02SourcePairResultV1 completed)
            {
                if (completed == null)
                    return "nc02_match_null_result";
                if (completed.CaptureIdentity == null)
                    return "nc02_match_null_identity";
                if (completed.SourceLeaseEpoch != 0UL)
                    return "nc02_match_source_lease_nonzero";
                // Local PairId only needs to be non-zero; must NOT be conflated with FrameSerial.
                // A coincidental equality of the two numeric values is not an error.
                if (completed.PairId == 0UL)
                    return "nc02_match_pair_id_zero";

                NC02XrQA.NC02XrCaptureIdentityV1 rid = completed.CaptureIdentity;
                if (rid.FrameSerial != Identity.FrameSerial)
                    return "nc02_match_ident_frame_serial";
                if (rid.PoseSerial != Identity.PoseSerial)
                    return "nc02_match_ident_pose_serial";
                if (rid.NativeXrEpoch != Identity.NativeXrEpoch)
                    return "nc02_match_ident_epoch";
                if (rid.OriginalGeneration != Identity.OriginalGeneration)
                    return "nc02_match_ident_orig_gen";
                if (rid.PoseSampleStartQpc != Identity.PoseSampleStartQpc)
                    return "nc02_match_ident_pose_start_qpc";
                if (rid.PoseSampleEndQpc != Identity.PoseSampleEndQpc)
                    return "nc02_match_ident_pose_end_qpc";
                if (rid.CaptureBeginQpc != Identity.CaptureBeginQpc)
                    return "nc02_match_ident_capture_begin_qpc";

                if (completed.FrameSerial != Identity.FrameSerial)
                    return "nc02_match_top_frame_serial";
                if (completed.NativePoseSerial != Identity.PoseSerial)
                    return "nc02_match_top_pose_serial";
                if (completed.NativeXrEpoch != Identity.NativeXrEpoch)
                    return "nc02_match_top_epoch";
                if (completed.OwnerGeneration != Identity.OriginalGeneration)
                    return "nc02_match_top_owner_gen";

                if (completed.Width != Width || completed.Height != Height)
                    return "nc02_match_dimensions";
                if (Stride != checked(Width * 4))
                    return "nc02_match_row_stride_invariant";
                if (completed.QualityGeneration != QualityGeneration ||
                    completed.QualityGeneration != 1 ||
                    QualityGeneration != 1)
                    return "nc02_match_quality_generation";
                if (completed.SourceFormat != 21u && completed.SourceFormat != 22u)
                    return "nc02_match_source_format";
                if (completed.Perspective != Perspective)
                    return "nc02_match_perspective";
                if (System.BitConverter.DoubleToInt64Bits(completed.Aspect) !=
                    System.BitConverter.DoubleToInt64Bits(Aspect))
                    return "nc02_match_aspect_bits";
                if (System.BitConverter.DoubleToInt64Bits(completed.LeftFovY) !=
                    System.BitConverter.DoubleToInt64Bits(SourceFovLeft))
                    return "nc02_match_left_fov_bits";
                if (System.BitConverter.DoubleToInt64Bits(completed.RightFovY) !=
                    System.BitConverter.DoubleToInt64Bits(SourceFovRight))
                    return "nc02_match_right_fov_bits";

                if (completed.LeftRawView16 == null || completed.RightRawView16 == null ||
                    completed.LeftRawView16.Length != 16 || completed.RightRawView16.Length != 16)
                    return "nc02_match_raw_view_shape";
                // Same-capture request-to-actual absolute owner contract: all 32 elements finite,
                // absolute delta <= 1e-6 per element, no transpose, no relative tolerance,
                // no axis guessing, no exposure of the private numeric arrays outside this class.
                double rawMaxAbsDelta = 0.0;
                for (int i = 0; i < 16; i++)
                {
                    double expected = _rawLeft16[i], actual = completed.LeftRawView16[i];
                    if (double.IsNaN(expected) || double.IsInfinity(expected) || double.IsNaN(actual) || double.IsInfinity(actual))
                        return "nc02_match_left_raw_nonfinite";
                    double delta = Math.Abs(actual - expected);
                    if (double.IsNaN(delta) || double.IsInfinity(delta) || delta > 1e-6)
                        return "nc02_match_left_raw_delta";
                    if (delta > rawMaxAbsDelta)
                        rawMaxAbsDelta = delta;
                }
                for (int i = 0; i < 16; i++)
                {
                    double expected = _rawRight16[i], actual = completed.RightRawView16[i];
                    if (double.IsNaN(expected) || double.IsInfinity(expected) || double.IsNaN(actual) || double.IsInfinity(actual))
                        return "nc02_match_right_raw_nonfinite";
                    double delta = Math.Abs(actual - expected);
                    if (double.IsNaN(delta) || double.IsInfinity(delta) || delta > 1e-6)
                        return "nc02_match_right_raw_delta";
                    if (delta > rawMaxAbsDelta)
                        rawMaxAbsDelta = delta;
                }

                if (completed.LeftBgra == null || completed.RightBgra == null)
                    return "nc02_match_bgra_null";
                long expectedPayload = checked((long)Stride * (long)Height);
                if (completed.LeftBgra.LongLength != expectedPayload ||
                    completed.RightBgra.LongLength != expectedPayload)
                    return "nc02_match_bgra_length";
                if (ReferenceEquals(completed.LeftBgra, completed.RightBgra))
                    return "nc02_match_bgra_alias";
                if (completed.NativeCollectedBytes != 176 ||
                    completed.NativeCollectedRawBytes == null ||
                    completed.NativeCollectedRawBytes.LongLength != 176L)
                    return "nc02_match_native_collected";
                if (completed.LeftSourceIdentity == 0UL ||
                    completed.RightSourceIdentity == 0UL ||
                    completed.DeviceIdentity == 0UL)
                    return "nc02_match_source_device_zero";
                if (string.IsNullOrEmpty(completed.MainBefore) ||
                    string.IsNullOrEmpty(completed.MainAfter) ||
                    !string.Equals(completed.MainBefore, completed.MainAfter, System.StringComparison.Ordinal))
                    return "nc02_match_main_strings";
                // commit the diagnostic only after every gate above has passed; failures never write it
                LastRawMatrixMaxAbsDelta = rawMaxAbsDelta;
                return null;
            }
        }

        public ulong FramesSubmitted { get { return _framesSubmitted; } }
        public int CameraChangeCount { get { return _cameraChangeCount; } }

        /// <summary>0.1.3.5 monitor: honest pipeline phase — "waiting" until the EntryCalibration
        /// gate opens, "calibrating" inside the stability window, "live" only after ENTRY_LOCKED.
        /// Never reports live without a lock (host MonitorPhase additionally blocks it on XR 7/8).
        /// hosted-d3d9 adds honest PRECEDENCE: capture_failed (recorded fault/timeout still current)
        /// → stopped (explicit Stop / invalidated generation) → await_hosted_ready (owned stage not
        /// Ready yet); only an actual same-generation Ready falls back to waiting/calibrating/live.
        /// The legacy default route is unchanged.</summary>
        internal string CurrentPhase
        {
            get
            {
                if (_hostedBackendSelected && _hostedStartGeneration >= 0)
                {
                    if (_hostedFailureGeneration == _hostedStartGeneration && _stopStatus == "capture_failed")
                        return "capture_failed";
                    var ho = _hostedOwner;
                    if (!_running || _generation != _hostedStartGeneration ||
                        ho == null || ho.BoundGeneration != _hostedStartGeneration)
                        return "stopped";
                    if (!string.IsNullOrEmpty(ho.FailureReason) || !ho.AdmissionOpen ||
                        _hostedFailureGeneration == _hostedStartGeneration)
                        return "capture_failed";
                    if (!IsCaptureBackendReady) return "await_hosted_ready";
                }
                var c = _cal;
                if (c == null) return "waiting";
                switch (c.State)
                {
                    case EntryCalState.CalibratingEntry: return "calibrating";
                    case EntryCalState.EntryLocked: return "live";
                    default: return "waiting";
                }
            }
        }
        internal ulong EyeFovSkipsForMonitor { get { return _fovInvalidSkips; } }
        internal int InvalidTrackingFramesForMonitor { get { return _invalidTrackingFrames; } }

        /// <summary>Monitor/QA: the projection pipeline FROZEN at construction from
        /// VRSIM_PROJECTION_PIPELINE — "gpu_worker_projection" only for the exact env value
        /// "gpu", otherwise "cpu_worker_resample" (the unchanged default).</summary>
        internal string ProjectionPipelineForMonitor
        {
            get { return _gpuPipeline ? "gpu_worker_projection" : "cpu_worker_resample"; }
        }

        /// <summary>QA helper: raw-GPU opt-in selection for this producer instance.</summary>
        internal bool GPUSelected { get { return _gpuPipeline; } }

        /// <summary>hosted-d3d9 selection FROZEN at construction — exact env
        /// VRSIM_CAPTURE_BACKEND="hosted-d3d9" only; anything else/unset is the legacy route.</summary>
        internal bool HostedSelected { get { return _hostedBackendSelected; } }

        /// <summary>The original Start generation captured by the current hosted Start
        /// (the value both the producer and the owner must still report for admission).</summary>
        internal int HostedStartGeneration { get { return _hostedStartGeneration; } }

        /// <summary>Pure managed admission gate — NO SDK calls in this getter. True only while
        /// running, the producer's CURRENT generation still equals the original hosted Start
        /// generation, the owner's BoundGeneration still equals it, and the owner's managed
        /// admission is open.</summary>
        internal bool CaptureAdmissionOpen
        {
            get
            {
                var o = _hostedOwner;
                return _running && o != null &&
                    _generation == _hostedStartGeneration &&
                    o.BoundGeneration == _hostedStartGeneration &&
                    o.AdmissionOpen;
            }
        }

        /// <summary>True only when the runtime backend is up, quality is READY
        /// (QualityGeneration==1) and no sticky failure is recorded. false while
        /// CaptureAdmissionOpen is true is the NORMAL pending state — never a start failure.</summary>
        internal bool IsCaptureBackendReady
        {
            get
            {
                var o = _hostedOwner;
                return o != null && o.RuntimeBackend &&
                    o.QualityGeneration == 1 && string.IsNullOrEmpty(o.FailureReason);
            }
        }

        /// <summary>The hosted owner's sticky failure reason ("" = none recorded).</summary>
        internal string CaptureBackendFailureReason
        {
            get
            {
                var o = _hostedOwner;
                var r = o == null ? null : o.FailureReason;
                return r ?? string.Empty;
            }
        }

        /// <summary>The original Start/owner-bound generation a hosted fault/timeout was recorded
        /// for (-1 = none). Identity is per hosted Start generation: a newer Start supersedes it.</summary>
        internal int HostedFailureGenerationForMonitor { get { return _hostedFailureGeneration; } }

        /// <summary>The producer-CACHED hosted failure reason ("" = none for the current hosted
        /// Start generation) — covers owner faults, unexpected admission closure, readiness
        /// exceptions and the 15 s readiness timeout, all of which the owner may not report.</summary>
        internal string HostedCaptureFailureReasonForMonitor
        {
            get
            {
                return _hostedStartGeneration >= 0 && _hostedFailureGeneration == _hostedStartGeneration
                    ? _hostedFailureReason : string.Empty;
            }
        }

        /// <summary>Async-notification permission for the later cards (09-reviewed03+10) — nothing
        /// consumes it yet. True only while the recorded failure belongs to the CURRENT hosted Start
        /// generation AND no explicit Stop has run since (StopAndRestore leaves _stopStatus
        /// "stopped", which cancels the stale notification honestly).</summary>
        internal bool HostedCaptureFailureNotificationPending
        {
            get
            {
                return _hostedFailureNotifyAllowed && _hostedStartGeneration >= 0 &&
                    _hostedFailureGeneration == _hostedStartGeneration &&
                    _stopStatus == "capture_failed";
            }
        }

        /// <summary>MONITORING-ONLY managed scheduler label: "kernel-render-120" for the exact
        /// VRSIM_CAPTURE_SCHEDULER=kernel-render-120 opt-in, "kernel-pulse" only when the exact
        /// VRSIM_CAPTURE_SCHEDULER=kernel-pulse opt-in is set, "coalesced-background" only when the
        /// exact VRSIM_CAPTURE_SCHEDULER opt-in is active on the frozen hosted-d3d9 + gpu route,
        /// otherwise "timer". NEVER an FPS claim — the nominal timer interval and the pulse chain
        /// are scheduling facts only; accepted pulses only prove enqueue acceptance.</summary>
        internal string CaptureSchedulerMode
        {
            get { return _kernelRender120Requested ? "kernel-render-120" : (_kernelPulseRequested ? "kernel-pulse" : (_coalescedBackgroundScheduler ? "coalesced-background" : "timer")); }
        }

        public VCFrameProducer(VRSIMPlugin host, Dispatcher disp)
        {
            _host = host;
            _disp = disp;
            // Exact strings ONLY (no trim, no numeric parse): "1" → controlled 1 ms opt-in,
            // "8" → 8 ms; anything else or unset → default 33 ms. Nominal timer interval
            // only — NOT an FPS guarantee.
            var tickEnv = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_TICK_MS");
            _tickMsEnvSelected = tickEnv == "8" || tickEnv == "1";
            _nominalTickMs = tickEnv == "1" ? 1.0 : (_tickMsEnvSelected ? 8.0 : 33.0);
            // Raw-GPU opt-in: EXACT string "gpu" ONLY (no trim, no case change); every other
            // value or unset keeps the CPU worker resample DEFAULT path.
            var pipeEnv = Environment.GetEnvironmentVariable("VRSIM_PROJECTION_PIPELINE");
            _gpuPipeline = pipeEnv == "gpu";
            // Hosted-d3d9 opt-in: EXACT string "hosted-d3d9" ONLY (no trim, no case change);
            // every other value or unset keeps the legacy default route.
            var backendEnv = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_BACKEND");
            _hostedBackendSelected = backendEnv == "hosted-d3d9";
            // Capture scheduler opt-in: EXACT string "coalesced-background" ONLY (no trim, no case
            // change), and only when this producer is ALSO frozen to hosted-d3d9 + gpu. Every other
            // value or unset keeps the plain DispatcherTimer DEFAULT. Nominal timer/pulse pacing is
            // NOT an FPS guarantee and no performance claim is made here.
            var schedEnv = Environment.GetEnvironmentVariable("VRSIM_CAPTURE_SCHEDULER");
            _coalescedBackgroundScheduler = schedEnv == "coalesced-background" &&
                backendEnv == "hosted-d3d9" && pipeEnv == "gpu";
            // Kernel-pulse scheduler opt-in: EXACT string "kernel-pulse" ONLY (no trim, no case
            // change, no parse) frozen here; compatibility is refused in Start BEFORE any SDK/owner
            // work. The exact value can never equal "coalesced-background", so at most ONE
            // scheduler source exists; every other value or unset keeps the plain timer DEFAULT.
            _kernelPulseRequested = schedEnv == "kernel-pulse";
            // Kernel-render-120 scheduler opt-in: EXACT string "kernel-render-120" ONLY (no trim,
            // no case change, no parse) frozen from the SAME constructor-local schedEnv. The exact
            // value can never equal "kernel-pulse" or "coalesced-background", so at most ONE
            // scheduler source exists; the shared + hosted-d3d9 + gpu + native-1536x1536 tuple is
            // enforced in Start BEFORE any owner/SDK work, and every other value or unset keeps the
            // plain DispatcherTimer DEFAULT unchanged.
            _kernelRender120Requested = schedEnv == "kernel-render-120";
            // NC02-XR staging opt-in: EXACT string "nc02-stage" ONLY (no trim, no case change);
            // every other value or unset keeps the current pipeline. P1 adds UNWIRED state only —
            // no staging API call, no behavior change. Managed pending limit stays frozen at 1.
            var readbackEnv = Environment.GetEnvironmentVariable("VRSIM_READBACK_PIPELINE");
            _nc02StagingRequested = readbackEnv == "nc02-stage";
            // OBS-SHARED typed GPU capture opt-in: EXACT string "obs-shared" ONLY (no trim, no case
            // change) from the SAME constructor-local readbackEnv. Mutually exclusive with nc02-stage by
            // construction. Never reread in Start/Tick/callback.
            _obsSharedRequested = readbackEnv == "obs-shared";
            _evidenceDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "KUKA", "iiQWorks.Sim", "VRSIM", "vc-frames");
            try { Directory.CreateDirectory(_evidenceDir); } catch { }
        }

        /// <summary>Compatibility overload: re-reads the LIVE camera. Only for callers without an
        /// Enter-time snapshot — the production path must use the (rawView, fovY, gaze) overload.</summary>
        public void CaptureEntryState()
        {
            _disp.Invoke((System.Action)(() =>
            {
                try
                {
                    _app = _host.Application;
                    var vp = VisualComponents.UX.Input.HelperViewport.ActiveViewport;
                    if (vp == null || vp.Window == null) { _host.Log("ENTRY_FAIL no_active_viewport"); return; }
                    _win = vp.Window;
                    _cam = _win.Camera;
                    double gaze = double.NaN;
                    try { gaze = _cam.GazeLength; } catch { }
                    ApplyEntrySnapshot(_cam.ViewMatrix, _cam.FieldOfViewY, gaze, "LIVE_REREAD");
                }
                catch (Exception ex) { _host.Log("ENTRY_FAIL " + ex.GetType().Name + ": " + ex.Message); }
            }));
        }

        /// <summary>Production entry: pass the RAW ViewMatrix/FOV/Gaze snapshot taken at Enter VR —
        /// the live producer must NEVER re-adopt a post-sweep camera.</summary>
        public void CaptureEntryState(Matrix rawEntryView, double fovY, double gazeLength)
        {
            _disp.Invoke((System.Action)(() =>
            {
                try
                {
                    _app = _host.Application;
                    var vp = VisualComponents.UX.Input.HelperViewport.ActiveViewport;
                    if (vp == null || vp.Window == null) { _host.Log("ENTRY_FAIL no_active_viewport"); return; }
                    _win = vp.Window;
                    _cam = _win.Camera;
                    ApplyEntrySnapshot(rawEntryView, fovY, gazeLength, "ENTER_SNAPSHOT");
                }
                catch (Exception ex) { _host.Log("ENTRY_FAIL " + ex.GetType().Name + ": " + ex.Message); }
            }));
        }

        private void ApplyEntrySnapshot(Matrix rawView, double fovY, double gazeLength, string source)
        {
            _entryView = rawView;   // RAW world-to-camera view — restored un-inverted
            _entryFovY = fovY;
            _entryGaze = gazeLength;
            _entryCaptured = true;
            _preCalibrationFrame = null;   // new entry snapshot → cached static preview is stale
            ClearHostedPreviewCaches();    // hosted static preview is stale on the SAME snapshot change
            MatToRotPos(_entryView, _R_E, _P_E);   // → CAMERA-TO-WORLD pose for the math below
            var ci = CultureInfo.InvariantCulture;
            _host.Log("VC_ENTRY_STATE source=" + source + " cam=(" +
                _P_E[0].ToString("0.0", ci) + "," +
                _P_E[1].ToString("0.0", ci) + "," +
                _P_E[2].ToString("0.0", ci) + ") fovY=" + fovY.ToString("0.0", ci) +
                " gaze=" + (double.IsNaN(gazeLength) || double.IsInfinity(gazeLength) ? "n/a" : gazeLength.ToString("0.0", ci)));
            _host.WriteEntryState(_entryView, _entryFovY);
        }

        public void Start()
        {
            if (_running) return;
            if (!_entryCaptured || _app == null || _win == null || _cam == null)
            {
                _host.Log("VC_PRODUCER start_refused entry/app/win/cam missing entryCaptured=" +
                    (_entryCaptured ? "true" : "false") + " app=" + (_app != null ? "true" : "false") +
                    " win=" + (_win != null ? "true" : "false") + " cam=" + (_cam != null ? "true" : "false"));
                // NEVER return silently: a silent refusal would let the host claim VRReady with no producer.
                throw new InvalidOperationException("VC_PRODUCER start refused — entry/app/win/cam missing (see VC_PRODUCER log)");
            }
            // OBS-SHARED gate — BEFORE any SDK/hosting/owner work, exact opt-in, NO fallback and NO
            // silent ignore. Shared is accepted ONLY on hosted-d3d9 + gpu + native-1536x1536
            // (frame exactly 1536x1536) + the plain DispatcherTimer OR the exact kernel-render-120
            // scheduler (kernel-pulse and coalesced-background are STILL refused), and only when
            // the frozen shared wire layout (64/192/32/72 +
            // Plan56/Stats120) validates. A later native capability/refusal stays honest — never a
            // legacy raw-GPU substitute. All exact requests then hit the hard activation gate below and
            // are refused before hosting until the Native2B/Managed3B lifecycle cards pass.
            if (_obsSharedRequested)
            {
                string sharedIncompatible = null;
                if (!_hostedBackendSelected || !_gpuPipeline)
                    sharedIncompatible = "VRSIM_CAPTURE_BACKEND=hosted-d3d9 AND VRSIM_PROJECTION_PIPELINE=gpu are required";
                else if (CaptureProfile.Selected != ObsSharedCaptureProfileV1 ||
                    FrameW != ObsSharedTicketEdgeV1 || FrameH != ObsSharedTicketEdgeV1)
                    sharedIncompatible = "capture profile " + ObsSharedCaptureProfileV1 + " with frame " +
                        ObsSharedTicketEdgeV1 + "x" + ObsSharedTicketEdgeV1 + " is required (frame=" +
                        FrameW + "x" + FrameH + " profile=" + CaptureProfile.Selected + ")";
                else if (_kernelPulseRequested || _coalescedBackgroundScheduler)
                    sharedIncompatible = "capture scheduler must be the plain timer or the exact kernel-render-120; kernel-pulse/coalesced-background are not allowed with obs-shared";
                else if (!VRSIMPlugin.SharedGpuWireLayoutValid())
                    sharedIncompatible = "shared GPU wire layout (ticket64/identity192/stop32/stats72 + Plan56/Stats120) validation failed";
                if (sharedIncompatible != null)
                {
                    _host.Log("VC_PRODUCER start_refused obs_shared_incompatible reason=" + sharedIncompatible + " no_fallback");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — VRSIM_READBACK_PIPELINE=obs-shared is incompatible (" +
                        sharedIncompatible + "). The exact opt-in is never silently ignored; no fallback is attempted.");
                }
                if (!ManagedObsSharedActivation)
                {
                    _host.Log("VC_PRODUCER start_refused obs_shared_activation_disabled");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — VRSIM_READBACK_PIPELINE=obs-shared compiled PREPARE/COMMIT/DISCARD hooks are present, " +
                        "but shared runtime activation is DISABLED until the Native2B worker/shutdown and Managed3B stop lifecycle cards pass " +
                        "independent review and compilation. No hosting or SDK call is attempted.");
                }
            }
            // Kernel-render-120 scheduler gate — BEFORE any SDK/owner work, exact opt-in, NO
            // fallback and NO silent ignore: kernel-render-120 is accepted ONLY on obs-shared +
            // hosted-d3d9 + gpu + native-1536x1536 (frame exactly 1536x1536). Every other tuple
            // THROWS here before hosting. The separate NC02 kernel-pulse gate below stays exact
            // and unchanged; the shared gate above still refuses kernel-pulse/coalesced.
            if (_kernelRender120Requested)
            {
                string render120Incompatible = null;
                if (!_obsSharedRequested)
                    render120Incompatible = "VRSIM_READBACK_PIPELINE=obs-shared is required";
                else if (!_hostedBackendSelected || !_gpuPipeline)
                    render120Incompatible = "VRSIM_CAPTURE_BACKEND=hosted-d3d9 AND VRSIM_PROJECTION_PIPELINE=gpu are required";
                else if (CaptureProfile.Selected != ObsSharedCaptureProfileV1 ||
                    FrameW != ObsSharedTicketEdgeV1 || FrameH != ObsSharedTicketEdgeV1)
                    render120Incompatible = "capture profile " + ObsSharedCaptureProfileV1 + " with frame " +
                        ObsSharedTicketEdgeV1 + "x" + ObsSharedTicketEdgeV1 + " is required (frame=" +
                        FrameW + "x" + FrameH + " profile=" + CaptureProfile.Selected + ")";
                if (render120Incompatible != null)
                {
                    _host.Log("VC_PRODUCER start_refused kernel_render120_incompatible reason=" + render120Incompatible + " no_fallback");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — VRSIM_CAPTURE_SCHEDULER=kernel-render-120 is incompatible (" +
                        render120Incompatible + "). The exact opt-in is never silently ignored; no fallback is attempted.");
                }
            }
            // NC02-XR staging gate — BEFORE any SDK/owner work, exact opt-in, NO fallback and
            // NO silent ignore: nc02-stage is accepted ONLY on hosted-d3d9 + gpu pipeline +
            // native-1536x1536 (frame exactly 1536x1536) + the plain DispatcherTimer scheduler.
            // coalesced-background must never mix with staging in this controlled validation.
            if (_nc02StagingRequested)
            {
                string nc02Incompatible = null;
                if (!_hostedBackendSelected || !_gpuPipeline)
                    nc02Incompatible = "VRSIM_CAPTURE_BACKEND=hosted-d3d9 AND VRSIM_PROJECTION_PIPELINE=gpu are required";
                else if (CaptureProfile.Selected != "native-1536x1536" || FrameW != 1536 || FrameH != 1536)
                    nc02Incompatible = "capture profile native-1536x1536 with frame 1536x1536 is required (frame=" +
                        FrameW + "x" + FrameH + " profile=" + CaptureProfile.Selected + ")";
                else if (_coalescedBackgroundScheduler)
                    nc02Incompatible = "capture scheduler must be timer; coalesced-background is not allowed with nc02-stage";
                if (nc02Incompatible != null)
                {
                    _host.Log("VC_PRODUCER start_refused nc02_stage_incompatible reason=" + nc02Incompatible + " no_fallback");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — VRSIM_READBACK_PIPELINE=nc02-stage is incompatible (" +
                        nc02Incompatible + "). The exact opt-in is never silently ignored; no fallback is attempted.");
                }
            }
            // Kernel-pulse scheduler gate — BEFORE any SDK/owner work, exact opt-in, NO fallback and
            // NO silent ignore: kernel-pulse is accepted ONLY on hosted-d3d9 + gpu pipeline +
            // nc02-stage + native-1536x1536 (frame exactly 1536x1536), and it must never mix with
            // the coalesced-background scheduler. The plain (non-kernel) NC02 timer route keeps the
            // existing coalesced refusal gate above unchanged.
            if (_kernelPulseRequested)
            {
                string kernelIncompatible = null;
                if (!_hostedBackendSelected || !_gpuPipeline)
                    kernelIncompatible = "VRSIM_CAPTURE_BACKEND=hosted-d3d9 AND VRSIM_PROJECTION_PIPELINE=gpu are required";
                else if (!_nc02StagingRequested)
                    kernelIncompatible = "VRSIM_READBACK_PIPELINE=nc02-stage is required";
                else if (CaptureProfile.Selected != "native-1536x1536" || FrameW != 1536 || FrameH != 1536)
                    kernelIncompatible = "capture profile native-1536x1536 with frame 1536x1536 is required (frame=" +
                        FrameW + "x" + FrameH + " profile=" + CaptureProfile.Selected + ")";
                else if (_coalescedBackgroundScheduler)
                    kernelIncompatible = "capture scheduler must not be coalesced-background with kernel-pulse";
                if (kernelIncompatible != null)
                {
                    _host.Log("VC_PRODUCER start_refused kernel_pulse_incompatible reason=" + kernelIncompatible + " no_fallback");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — VRSIM_CAPTURE_SCHEDULER=kernel-pulse is incompatible (" +
                        kernelIncompatible + "). The exact opt-in is never silently ignored; no fallback is attempted.");
                }
            }
            bool hostedRoute = false;
            _disp.Invoke((System.Action)(() =>
            {
                // hosted-d3d9 runtime backend: an INDEPENDENT branch BEFORE the legacy
                // NativeCaptureOwner construction below. Incompatible configurations are refused
                // inside StartHosted, before hosting — no silent fallback.
                if (_hostedBackendSelected)
                {
                    hostedRoute = true;
                    StartHosted();
                    return;
                }
                // The legacy NativeCaptureOwner route must NEVER run the high-res native
                // profiles — hosted-d3d9 is required for them; no silent fallback.
                if (CaptureProfile.Selected == "native-1536x1536" || CaptureProfile.Selected == "native-2048x2048")
                {
                    _host.Log("VC_PRODUCER start_refused legacy_route_highres_profile profile=" +
                        CaptureProfile.Selected + " hosted_d3d9_required no_fallback");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — capture profile " + CaptureProfile.Selected +
                        " requires VRSIM_CAPTURE_BACKEND=hosted-d3d9; the legacy NativeCaptureOwner route does not support it. No fallback is attempted.");
                }
                _preCalibrationFrame = null;   // fresh session — no stale preview
                // A retained worker from a previous stop that has NOT exited yet is a live
                // background thread — refuse the new session rather than run two workers.
                if (_unjoinedWorker != null)
                {
                    if (!_unjoinedWorker.HasExited)
                    {
                        _host.Log("VC_PRODUCER start_refused previous_worker_not_exited");
                        throw new InvalidOperationException(
                            "VC_PRODUCER start refused — previous StereoResampleWorker has not exited (retry StopAndRestore)");
                    }
                    _unjoinedWorker.Stop(500);   // thread already dead → clears worker state, returns true
                    _unjoinedWorker = null;
                }
                // An owner RETAINED from a failed cleanup still holds the recording/renderer
                // state — refuse the start; the retained _nativeOwner must never be overwritten.
                if (_nativeOwner != null)
                {
                    _host.Log("VC_PRODUCER start_refused retained_owner");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — NativeCaptureOwner retained from a failed cleanup, retry StopAndRestore first");
                }
                var vp = Caliburn.Micro.IoC.Get<VisualComponents.Create3D.IViewport>(null);
                if (vp == null || !ReferenceEquals(vp.Window, _win))
                {
                    _host.Log("VC_PRODUCER start_refused viewport_window_mismatch");
                    throw new InvalidOperationException("VC_PRODUCER start refused — active viewport window mismatch");
                }
                var rs = Caliburn.Micro.IoC.Get<IRenderService>(null);
                if (rs == null)
                {
                    _host.Log("VC_PRODUCER start_refused render_service_missing");
                    throw new InvalidOperationException("VC_PRODUCER start refused — render service missing");
                }
                // GPU opt-in: the raw-GPU native API must be SUPPORTED BEFORE any owner
                // mutation. NO silent CPU fallback — an unsupported wire aborts Start clearly.
                if (_gpuPipeline && !_host.RawGpuApiSupported())
                {
                    _host.Log("VC_PRODUCER start_refused gpu_api_unsupported no_silent_cpu_fallback");
                    throw new InvalidOperationException(
                        "VC_PRODUCER start refused — VRSIM_PROJECTION_PIPELINE=gpu but the native raw-GPU " +
                        "API is unsupported (RawGpuApiSupported=false). Unset the variable for the CPU resample pipeline.");
                }
                var owner = new NativeCaptureOwner(_app, vp, rs, _win, _cam, FrameW, FrameH);
                Exception beginError = null;
                try { owner.Begin(); }
                catch (Exception bex) { beginError = bex; }
                if (!owner.Begun)
                {
                    // Begin failed → NO timer, NO running state; the error PROPAGATES, never swallowed.
                    _host.Log("VC_PRODUCER begin_failed diag=" + owner.DiagnosticText);
                    Exception disposeError = null;
                    try { owner.Dispose(); }
                    catch (Exception dex)
                    {
                        disposeError = dex;
                        _host.Log("VC_PRODUCER begin_cleanup_fail " + dex.GetType().Name + ": " + dex.Message);
                    }
                    if (disposeError != null)
                    {
                        // Cleanup unverified → retain the failed owner so exit (StopAndRestore) can retry.
                        _nativeOwner = owner;
                        var inners = new System.Collections.Generic.List<Exception>();
                        if (beginError != null) inners.Add(beginError);
                        inners.Add(disposeError);
                        throw new AggregateException(
                            "VC_PRODUCER begin failed and owner cleanup unverified — owner retained for exit retry", inners);
                    }
                    if (beginError != null)
                        throw new AggregateException("VC_PRODUCER begin failed", beginError);
                    throw new InvalidOperationException(
                        "VC_PRODUCER begin failed — owner not begun: " + owner.DiagnosticText);
                }
                _nativeOwner = owner;   // sole recording/renderer owner — begun BEFORE the worker/timer start

                // ONE worker per Start generation; bounded state, pure CPU, no SDK/native/UI/IO.
                StereoResampleWorker worker = null;
                Exception startError = null;
                try
                {
                    _generation++;
                    if (_gpuPipeline)
                    {
                        // GPU generation: NO StereoResampleWorker is created — native projects on
                        // the XR side; _worker/_lastWorker stay null for this generation.
                        _worker = null;
                        _lastWorker = null;
                    }
                    else
                    {
                        worker = new StereoResampleWorker(_generation);
                        _worker = worker;
                        _lastWorker = worker;   // worker-state snapshot source for the final telemetry flush
                    }
                    _captureSerial = 0UL;
                    _frameSerial = 0UL;
                    _lastCompletedFrameSerial = 0UL;
                    _captureMeta.Clear(); _captureMetaOrder.Clear();
                    _jobRejectLogCount = 0;
                    _stopStatus = "running";
                    ResetAsyncTelemetry();
                    ResetGpuTelemetry();   // both telemetry sets clear on every Start
                    _timer = new DispatcherTimer(DispatcherPriority.Render, _disp) { Interval = TimeSpan.FromMilliseconds(_nominalTickMs) }; // controlled priority + nominal-interval experiment (VRSIM_CAPTURE_TICK_MS) only — no claimed FPS gain
                    _timer.Tick += (s, e) => Tick();
                    _timer.Start();
                }
                catch (Exception sex)
                {
                    // Worker or timer failed AFTER Begin → stop the worker (bounded), clean up the
                    // owner, and PROPAGATE. Any cleanup that cannot be verified is RETAINED for retry.
                    startError = sex;
                    _host.Log("VC_PRODUCER start_fail " + sex.GetType().Name + ": " + sex.Message);
                    if (worker != null)
                    {
                        bool joined = false;
                        Exception wex = null;
                        try { joined = worker.Stop(500); }
                        catch (Exception wx) { wex = wx; }
                        _worker = null;
                        if (!joined || wex != null)
                        {
                            _unjoinedWorker = worker;   // retained for the exit retry — never Abort
                            _stopStatus = "worker_stop_timeout_retained";
                            startError = new AggregateException(startError,
                                wex ?? (Exception)new TimeoutException(
                                    "StereoResampleWorker did not exit within 500 ms during start cleanup — retained for retry"));
                        }
                    }
                    _timer = null;
                    Exception disposeError = null;
                    try { owner.Dispose(); }
                    catch (Exception dex)
                    {
                        disposeError = dex;
                        _host.Log("VC_PRODUCER start_cleanup_fail " + dex.GetType().Name + ": " + dex.Message);
                    }
                    if (disposeError != null)
                    {
                        // Owner cleanup unverified → retain the owner so exit can retry restore/dispose.
                        throw new AggregateException(
                            "VC_PRODUCER start (worker/timer) failed and owner cleanup unverified — owner retained for exit retry",
                            startError, disposeError);
                    }
                    _nativeOwner = null;
                    if (!ReferenceEquals(startError, sex)) throw startError;
                    throw;
                }
            }));
            if (hostedRoute) return;   // StartHosted already set/cleared _running and logged — the
                                       // legacy unconditional _running=true below must NOT run for it
            _running = true;
            _host.Log("VC_PRODUCER started (02BC live tracking + real stereo, " + FrameW + "x" + FrameH + ", native owner begun)" +
                " pipeline=" + ProjectionPipelineForMonitor +
                " nominalTickMs=" + _nominalTickMs.ToString("0.##", CultureInfo.InvariantCulture) +
                " tickMsSource=" + (_tickMsEnvSelected ? "env_VRSIM_CAPTURE_TICK_MS" : "default_33"));
        }

        /// <summary>hosted-d3d9 Start (UI dispatcher thread only). NO NativeCaptureOwner, NO
        /// StereoResampleWorker: _nativeOwner/_worker/_lastWorker stay null for this generation.
        /// A canceled/failed hosted Start THROWS (never a silent return — the outer Host would then
        /// announce ReadyReached for an already-stopped flow): a reentrant Stop after the factory
        /// throws OperationCanceledException with the new owner's managed gate closed via
        /// RequestStop(originalGeneration); Begin/setup failures RETAIN _hostedOwner, close the
        /// managed gate and propagate — no Dispose of the official window, no native Stop/Destroy
        /// here. A valid PENDING Start starts the dispatcher timer and returns; Ready is polled by
        /// the later card, never waited on synchronously.</summary>
        private void StartHosted()
        {
            // A RETAINED kernel pulse (pending/unverified stop) forbids any new hosted Start: the old
            // pulse/task/result refs must never be overwritten or restarted behind an unfinished Stop.
            if (_kernelPulse != null)
            {
                _host.Log("VC_PRODUCER start_refused kernel_pulse_retained");
                throw new InvalidOperationException(
                    "VC_PRODUCER start refused — KernelUiPulse retained from a previous session whose stop is not yet verified; retry StopAndRestore first");
            }
            if (_hostedOwner != null)
            {
                _host.Log("VC_PRODUCER start_refused retained_hosted_owner");
                throw new InvalidOperationException(
                    "VC_PRODUCER start refused — NC01HostedSceneOwner retained from a failed hosted start; retry the later Stop/Destroy card first");
            }
            if (_nativeOwner != null)
            {
                _host.Log("VC_PRODUCER start_refused retained_owner");
                throw new InvalidOperationException(
                    "VC_PRODUCER start refused — NativeCaptureOwner retained from a failed cleanup, retry StopAndRestore first");
            }
            // OBS-SHARED retained-ticket refusal — EXPLICIT, not inferred from "proven empty": any
            // entry still locally-owned, COMMITTED-but-unread, or holding a nonzero ticket from a
            // previous generation forbids a new Start. Such packets must NEVER be overwritten or reset
            // to permit a Start — the Managed3B stop card exact-Discards them first. Old storage is
            // never cleared here just to make a receipt clean.
            if (_obsSharedRequested)
            {
                for (int i = 0; i < _obsSharedOwned.Length; i++)
                {
                    if (_obsSharedOwned[i] || _obsSharedCommitted[i] || _obsSharedTickets[i].ticket != 0UL)
                    {
                        _host.Log("VC_PRODUCER start_refused obs_shared_retained_ticket slot=" + i +
                            " owned=" + (_obsSharedOwned[i] ? 1 : 0) +
                            " committed=" + (_obsSharedCommitted[i] ? 1 : 0) +
                            " ticket=" + _obsSharedTickets[i].ticket);
                        throw new InvalidOperationException(
                            "VC_PRODUCER start refused — an OBS-SHARED PREPARED/COMMITTED ticket is still retained from a previous generation; the later Managed3B stop card must resolve it first");
                    }
                }
            }
            // Pre-hosting compatibility gates — exact, NO fallback: hosted-d3d9 rides the raw-GPU
            // producer and only the three native capture profiles whose size matches the frame
            // exactly: native-1024x1024+1024x1024, native-1536x1536+1536x1536,
            // native-2048x2048+2048x2048.
            string incompatible = null;
            if (!_gpuPipeline)
                incompatible = "VRSIM_PROJECTION_PIPELINE=gpu is required";
            else if ((CaptureProfile.Selected != "native-1024x1024" || FrameW != 1024 || FrameH != 1024)
                && (CaptureProfile.Selected != "native-1536x1536" || FrameW != 1536 || FrameH != 1536)
                && (CaptureProfile.Selected != "native-2048x2048" || FrameW != 2048 || FrameH != 2048))
                incompatible = "capture profile native-1024x1024/native-1536x1536/native-2048x2048 with exactly matching frame size is required";
            else if (!_host.RawGpuApiSupported())
                incompatible = "RawGpuApiSupported=false";
            if (incompatible != null)
            {
                _host.Log("VC_PRODUCER start_refused hosted_backend_incompatible reason=" + incompatible + " no_fallback");
                throw new InvalidOperationException(
                    "VC_PRODUCER start refused — VRSIM_CAPTURE_BACKEND=hosted-d3d9 is incompatible (" +
                    incompatible + "). No silent fallback is attempted.");
            }

            ClearHostedPreviewCaches();   // fresh hosted generation — no stale preview caches
            _hostedStopBoundGeneration = -1;
            _hostedStopPending = false;
            _preCalibrationFrame = null;
            _generation++;
            var originalGeneration = _generation;
            _hostedStartGeneration = originalGeneration;
            _running = true;
            _worker = null;
            _lastWorker = null;
            _captureSerial = 0UL;
            _frameSerial = 0UL;
            _lastCompletedFrameSerial = 0UL;
            _captureMeta.Clear(); _captureMetaOrder.Clear();
            _jobRejectLogCount = 0;
            _stopStatus = "running";
            ResetAsyncTelemetry();
            ResetGpuTelemetry();

            // OBS-SHARED per-generation reset — reached ONLY after the explicit retained-ticket refusal
            // above (owned/committed/nonzero all rejected), so the ledger is proven empty here. Pure
            // managed Array.Clear on the three-entry ledger + committed flags plus numeric-counter resets;
            // the thin wrapper clears stale monitor-preview bytes WITHOUT producing replacement bytes.
            // No SDK, no native, no staging/prepare call, no IO.
            if (_obsSharedRequested)
            {
                _host.ClearSharedGpuPreviewCache();
                Array.Clear(_obsSharedTickets, 0, _obsSharedTickets.Length);
                Array.Clear(_obsSharedIdentities, 0, _obsSharedIdentities.Length);
                Array.Clear(_obsSharedOwned, 0, _obsSharedOwned.Length);
                Array.Clear(_obsSharedCommitted, 0, _obsSharedCommitted.Length);
                _obsSharedPrepared = 0UL;
                _obsSharedCommitAccepted = 0UL;
                _obsSharedRejected = 0UL;
                // Card3B1 stop-lifecycle reset — reached ONLY after the retained-ticket refusal above
                // proves the ledger empty, and never while a previous shared stop is unverified/terminal
                // (that state keeps Start blocked through the same refusal). Pure managed field resets.
                _obsSharedStopOwner = null;
                _obsSharedStopBoundGeneration = -1;
                _obsSharedStopBoundEpoch = 0UL;
                _obsSharedStopEntered = false;
                _obsSharedDrainAttempted = false;
                _obsSharedDrainRequestRc = int.MinValue;
                _obsSharedStopError = null;
                _obsSharedSourceStopSteps = 0;
                _obsSharedSourceStopBeginQpc = 0L;
                _obsSharedStopTerminal = false;
                _obsSharedSourceReady = false;
                _obsSharedWholeStopVerified = false;
                _obsSharedNeverBoundSourceReady = false;
                _obsSharedFinalStopState = default(VRSIMPlugin.SharedGpuStopStateV1);
                _obsSharedFinalStopStateValid = false;
            }

            // NC02-XR P2: per-generation staging-state reset — reached ONLY after the retained
            // hosted/native owner refusals above, so an old owner with pending handles or a live
            // generation is never reset here. Pure managed Clear/Array.Clear on the frozen 2-slot
            // arrays plus numeric-counter resets; no SDK, no staging API call, no IO.
            if (_nc02StagingRequested)
            {
                _nc02CaptureRows.Clear();
                Array.Clear(_nc02MapPairIds, 0, _nc02MapPairIds.Length);
                Array.Clear(_nc02MapFrameSerials, 0, _nc02MapFrameSerials.Length);
                Array.Clear(_nc02PollCounts, 0, _nc02PollCounts.Length);
                _nc02DrainBeginQpc = 0L;
                _nc02DrainAttempts = 0;
                _nc02DrainBudgetExpired = false;
                _nc02SubmitAccepted = 0UL;
                _nc02Collected = 0UL;
                _nc02NativeEnqueueAccepted = 0UL;
                _nc02PendingPolls = 0UL;
                _nc02Dropped = 0UL;
                _nc02LastSourceLatencyMs = double.NaN;
                _nc02LastPoseToEnqueueMs = double.NaN;
                lock (_nc02DiagLock)
                {
                    _nc02DiagGeneration = -1;
                    _nc02DiagLastKnown = false;
                    _nc02DiagLastPairId = 0UL;
                    _nc02DiagLastFrameSerial = 0UL;
                    _nc02DiagLastPoseSerial = 0UL;
                    _nc02DiagLastNativeXrEpoch = 0UL;
                    _nc02DiagLastSubmitNativeMs = double.NaN;
                    _nc02DiagLastCollectNativeMs = double.NaN;
                    _nc02DiagLastEyeLockMs = double.NaN;
                    _nc02DiagLastSurfaceRenderMs = double.NaN;
                    _nc02DiagLastPairPrepareMs = double.NaN;
                    _nc02DiagLastPairFinalizeMs = double.NaN;
                    _nc02DiagSumSubmitNativeMs = double.NaN;
                    _nc02DiagSumCollectNativeMs = double.NaN;
                    _nc02DiagSumEyeLockMs = double.NaN;
                    _nc02DiagSumSurfaceRenderMs = double.NaN;
                    _nc02DiagSumPairPrepareMs = double.NaN;
                    _nc02DiagSumPairFinalizeMs = double.NaN;
                    _nc02DiagLastGetRtdTicks = 0UL;
                    _nc02DiagLastIssueTicks = 0UL;
                    _nc02DiagLastQueryTicks = 0UL;
                    _nc02DiagLastLockTicks = 0UL;
                    _nc02DiagLastRowCopyTicks = 0UL;
                    _nc02DiagLastUnlockTicks = 0UL;
                    _nc02DiagLastSubmitTotalTicks = 0UL;
                    _nc02DiagLastPollTotalTicks = 0UL;
                    _nc02DiagLastNativeSubmitQpc = 0UL;
                    _nc02DiagLastNativeCollectQpc = 0UL;
                    _nc02DiagLastCaptureBeginQpc = 0L;
                    _nc02DiagLastCaptureEndQpc = 0L;
                    _nc02DiagLastQueryAttempts = 0UL;
                    _nc02DiagLastLeftLockAttempts = 0UL;
                    _nc02DiagLastRightLockAttempts = 0UL;
                    _nc02DiagSumQueryAttempts = 0UL;
                    _nc02DiagSumLeftLockAttempts = 0UL;
                    _nc02DiagSumRightLockAttempts = 0UL;
                    _nc02DiagObservedCollected = 0L;
                    _nc02DiagValidCommits = 0L;
                    _nc02DiagInvalidCommits = 0L;
                }
            }

            NC04.NC01HostedSceneOwner owner = null;
            try
            {
                owner = NC04.NC01HostedSceneOwner.CreateRuntimeOwner(_app, FrameW, FrameH);
                _hostedOwner = owner;   // stored BEFORE BeginHosting so a reentrant Stop sees it
                if (!_running || _generation != originalGeneration)
                    throw new OperationCanceledException(
                        "VC_PRODUCER hosted start canceled — a Stop reentered between the owner factory and BeginHosting");

                owner.BeginHosting(originalGeneration, () => !_running || _generation != originalGeneration);

                if (!_running || _generation != originalGeneration ||
                    owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen)
                    throw new OperationCanceledException(
                        "VC_PRODUCER hosted start canceled — invalid after BeginHosting (running/generation/boundGeneration/admission)");

                if (_kernelPulseRequested || _kernelRender120Requested)
                {
                    // Kernel-pulse / kernel-render-120 branch — this route NEVER creates/starts
                    // _timer and never mixes with coalesced-background. The UI thread reads the
                    // ACTUAL XR session epoch via ReadGpuPipelineStats (kernel callbacks may never
                    // call it); sessionEpoch 0 is invalid → throw, no fallback. The Action closure
                    // freezes originalGeneration, this exact owner and that epoch; _kernelPulse is
                    // saved BEFORE TryStart so any failure path can request the async stop while
                    // refs stay retained. The frozen renderPaced argument selects the old
                    // Background/unpaced mode (kernel-pulse, NC02 — bool false, unchanged) or the
                    // new Render + max 120 UI-action starts/second mode (kernel-render-120,
                    // obs-shared — bool true). ONE scheduler only; identical pre/post TryStart
                    // cancel checks for both.
                    KernelUiPulse pulse = null;
                    try
                    {
                        var stats = _host.ReadGpuPipelineStats();
                        if (stats == null || stats.Value.sessionEpoch == 0UL)
                            throw new InvalidOperationException(
                                "VC_PRODUCER kernel pulse start refused — ReadGpuPipelineStats sessionEpoch is absent/zero on the UI thread; no fallback");
                        ulong xrEpoch = stats.Value.sessionEpoch;

                        if (!_running || _generation != originalGeneration ||
                            !ReferenceEquals(_hostedOwner, owner) ||
                            owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen)
                            throw new OperationCanceledException(
                                "VC_PRODUCER kernel pulse start canceled — invalid before pulse creation (running/generation/owner/boundGeneration/admission)");

                        var pulseOwner = owner;
                        pulse = new KernelUiPulse(_disp,
                            () => OnKernelPulse(originalGeneration, pulseOwner, xrEpoch),
                            _kernelRender120Requested);
                        _kernelPulse = pulse;   // saved BEFORE TryStart — failure paths keep refs stoppable
                        // kernel-render-120: the pulse reference EXISTS from this assignment onward —
                        // the shared first-stop freeze must never claim an honest neverCreated.
                        if (_kernelRender120Requested)
                            _kernelRender120PulseEverCreated = true;

                        if (!_running || _generation != originalGeneration ||
                            !ReferenceEquals(_hostedOwner, owner) ||
                            owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen)
                            throw new OperationCanceledException(
                                "VC_PRODUCER kernel pulse start canceled — invalid after pulse creation, before TryStart");

                        string pulseReason;
                        if (!pulse.TryStart(out pulseReason))
                            throw new InvalidOperationException(
                                "VC_PRODUCER kernel pulse TryStart refused — reason=" + (pulseReason ?? "none") + "; no fallback");

                        if (!_running || _generation != originalGeneration ||
                            !ReferenceEquals(_hostedOwner, owner) ||
                            owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen)
                            throw new OperationCanceledException(
                                "VC_PRODUCER kernel pulse start canceled — invalid immediately after TryStart");

                        _host.Log("VC_PRODUCER hosted_capture_scheduler mode=" + CaptureSchedulerMode +
                            " xrEpoch=" +
                            xrEpoch.ToString(CultureInfo.InvariantCulture) +
                            (_kernelRender120Requested ?
                                " callback_priority=Render max_ui_action_starts_per_second=120 pacing_is_scheduler_fact_not_FPS" :
                                string.Empty) +
                            " ui_dispatched_pulse no_timer no_sleep no_busywait scheduler_labels_are_not_FPS");
                    }
                    catch (Exception)
                    {
                        // Loss/failure AFTER a pulse exists: request the async stop (non-blocking,
                        // no SDK) and propagate — the outer catch closes the managed owner gate and
                        // never clears pulse/task refs early.
                        try { RequestKernelPulseStop(); } catch { }
                        throw;
                    }
                }
                else
                {
                    _timer = new DispatcherTimer(DispatcherPriority.Render, _disp) { Interval = TimeSpan.FromMilliseconds(_nominalTickMs) };
                    _timer.Tick += (s, e) =>
                    {
                        // The RETAINED timer may only drive its OWN Start generation: a stale timer
                        // surviving a Stop/generation++ must never Tick a new generation.
                        if (_generation != originalGeneration || !_running) return;
                        // Coalesced-background opt-in: skip this beat while the single retained
                        // Background pulse is pending/executing — the pulse chain drives the captures;
                        // the timer stays the backoff driver whenever no token is held.
                        if (_coalescedPulseToken != null) return;
                        Tick();
                    };

                    if (!_running || _generation != originalGeneration ||
                        owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen)
                        throw new OperationCanceledException(
                            "VC_PRODUCER hosted start canceled — invalid immediately before timer start");
                    _timer.Start();
                    if (_coalescedBackgroundScheduler)
                        _host.Log("VC_PRODUCER hosted_capture_scheduler mode=coalesced-background nominalTimerMs=" +
                            _nominalTickMs.ToString("0", CultureInfo.InvariantCulture) +
                            " single_background_pulse no_0ms_timer no_sleep no_busywait scheduler_labels_are_not_FPS");
                }
            }
            catch (Exception ex)
            {
                // NO legacy Dispose here and NO legacy owner touched: the failed hosted owner is
                // RETAINED; only its MANAGED gate is closed, via RequestStop(originalGeneration) —
                // never CloseAdmission. Cancel checks above land here with the gate closed before
                // the OperationCanceledException propagates.
                _running = false;
                if (_timer != null)
                {
                    try { _timer.Stop(); } catch { }
                    _timer = null;
                }
                if (owner != null)
                {
                    try { owner.RequestStop(originalGeneration); }
                    catch (Exception rex)
                    {
                        _host.Log("VC_PRODUCER hosted_gate_close_fail " + rex.GetType().Name + ": " + rex.Message);
                    }
                }
                // Kernel-pulse opt-in: ASYNC stop request only (non-blocking, never SDK). The
                // pulse/task/result refs are NOT cleared here — the retained refs plus the closed
                // managed gate carry the failure until StopAndRestore verifies the stop.
                try { RequestKernelPulseStop(); } catch { }
                _host.Log("VC_PRODUCER hosted_start_fail generation=" + originalGeneration +
                    " ownerRetained=" + (owner != null ? "true" : "false") +
                    " " + ex.GetType().Name + ": " + ex.Message);
                throw;
            }

            _host.Log("VC_PRODUCER hosted_start generation=" + originalGeneration +
                " runtimeBackend=" + (owner.RuntimeBackend ? "true" : "false") +
                " boundGeneration=" + owner.BoundGeneration +
                " admissionOpen=" + (owner.AdmissionOpen ? "true" : "false") +
                " qualityGeneration=" + owner.QualityGeneration +
                " ready=" + (IsCaptureBackendReady ? "true" : "false") +
                " failureReason=" + (string.IsNullOrEmpty(owner.FailureReason) ? "none" : owner.FailureReason) +
                " nominalTickMs=" + _nominalTickMs.ToString("0.##", CultureInfo.InvariantCulture) +
                " (false+admissionOpen=true is the normal PENDING state — Ready is polled later, never waited on here)");
        }

        public void StopAndRestore()
        {
            // SINGLE dispatcher block: timer stop → generation INVALIDATED first → bounded worker
            // stop (unjoined worker RETAINED for retry, timeout propagated honestly) → owner
            // restore/dispose. The owner restore is ATTEMPTED EVEN IF the worker stop fails:
            // SDK/desktop restoration must never be skipped because of a background-thread timeout.
            // After a verified owner restore there is NO manual _cam restoration — it would
            // duplicate mutations and can invalidate the just-verified snapshot.
            _disp.Invoke((System.Action)(() =>
            {
                if (_timer != null) { _timer.Stop(); _timer = null; }   // stop even if _running is already false
                _running = false;
                // OBS-SHARED (Card3B1) first-Stop freeze — BEFORE the _generation++ invalidation, via
                // the SAME pure-managed/native-metadata helper the capture-failure path uses. No
                // owner/native/bridge mutation happens here.
                FreezeObsSharedFirstStopOnce();
                _generation++;                    // FIRST invalidation — a stopped generation can never submit again
                _preCalibrationFrame = null;      // session over — drop the cached preview
                _stopStatus = "stopped";
                // OBS-SHARED (Card3B1): close the frozen-original managed gate and request the
                // metadata-only native drain IMMEDIATELY in this same stop call, after the generation
                // invalidation and before any new submit could run. The bounded finite source steps
                // stay on the host deferred retry — never Discard/Finalize/Confirm from here.
                CloseObsSharedFirstStopAdmissionOnce();

                // Coalesced-background opt-in: AFTER the running=false/generation++ invalidation
                // above and BEFORE any SDK behavior below, cancel this token's PENDING pulse
                // Operation. An EXECUTING pulse keeps its token until its own finally; the
                // in-flight capture stays behind the existing _tickInProgress / owner StopVerified
                // gates — NO new NativeStop/Destroy. A finished old callback can never revive the
                // timer or a new generation: its token/current-generation checks all fail.
                CancelPendingHostedPulse();

                // Kernel-pulse opt-in — BEFORE any hosted/SDK cleanup and AFTER the timer stop →
                // running=false → generation++ invalidation above: save the owner's OWN
                // BoundGeneration at first Stop entry (never the already-incremented producer
                // generation), close admission against that original bound owner, and request the
                // pulse stop ASYNCHRONOUSLY (no Task.Result/.Wait anywhere). A not-yet-published or
                // unverified stop THROWS so Host.StopProducerIfAny keeps producer+owner+pulse refs
                // for the exact-source 100 ms retry — StopHostedCore is never entered on a pending
                // or unverified kernel stop. With no pulse the legacy Stop behavior is unchanged.
                // OBS-SHARED kernel-render-120: this OLD synchronous kernel gate applies ONLY to the
                // nonshared NC02 route. The shared route converges exclusively through the gated
                // TryStepObsSharedSourceStop / ConfirmObsSharedNativeDestroyed paths and RETAINS
                // pulse/task/result/error proof through WholeStop and until the process ends —
                // refs are never cleared here; the NC02 clearing below is unchanged.
                if (!_obsSharedRequested && _kernelPulse != null)
                {
                    var ko = _hostedOwner;
                    if (ko != null && _hostedStopBoundGeneration < 0)
                        _hostedStopBoundGeneration = ko.BoundGeneration;
                    if (ko != null && _hostedStopBoundGeneration > 0)
                    {
                        try { ko.RequestStop(_hostedStopBoundGeneration); }
                        catch (Exception krex)
                        {
                            _host.Log("VC_PRODUCER kernel_stop_gate_close_fail " + krex.GetType().Name + ": " + krex.Message);
                        }
                    }
                    RequestKernelPulseStop();   // non-blocking, idempotent same-task
                    var kres = _kernelPulseStopResult;
                    if (kres == null)
                    {
                        _stopStatus = "kernel_stop_pending";
                        throw new InvalidOperationException(
                            "VC_PRODUCER kernel stop pending — KernelUiPulse StopAsync result is not yet published" +
                            " (stopError=" + (_kernelPulseStopError ?? "none") +
                            "). Producer/owner/pulse/task refs RETAINED; no StopHostedCore/SDK cleanup this turn.");
                    }
                    if (!string.IsNullOrEmpty(_kernelPulseStopError) || kres.Failure != null || !kres.StopVerified ||
                        !kres.WorkerExited || kres.CallbackPending || kres.CallbackExecuting || !kres.HandlesReleased)
                    {
                        _stopStatus = "kernel_stop_unverified";
                        throw new InvalidOperationException(
                            "VC_PRODUCER kernel stop unverified — stopVerified=" + (kres.StopVerified ? "true" : "false") +
                            " workerExited=" + (kres.WorkerExited ? "true" : "false") +
                            " callbackPending=" + (kres.CallbackPending ? "true" : "false") +
                            " callbackExecuting=" + (kres.CallbackExecuting ? "true" : "false") +
                            " handlesReleased=" + (kres.HandlesReleased ? "true" : "false") +
                            " timedOut=" + (kres.TimedOut ? "true" : "false") +
                            " failure=" + (kres.Failure == null ? "none" : kres.Failure.ToString()) +
                            " stopError=" + (_kernelPulseStopError ?? "none") +
                            " — every producer/owner/pulse/task/result ref RETAINED; SDK cleanup refused.");
                    }
                    // Fully verified quiet stop — ONLY now clear the kernel refs, then fall through
                    // into the existing StopHostedCore path.
                    _kernelPulse = null;
                    _kernelPulseStopTask = null;
                    _kernelPulseStopResult = null;
                    _kernelPulseStopError = null;
                }

                // ---- hosted-d3d9 stop branch (reviewed08): the invalidation order above is KEPT
                // (timer stop → running=false → generation++ first). NO StereoResampleWorker and NO
                // NativeCaptureOwner exist on this route — the legacy worker-stop/restore/dispose
                // block below is never entered: no main-camera/size/recording/render/quality
                // mutation, no SDK eye-window/view/D3DImage Dispose and no helper unload. The owner
                // reference is RETAINED by design (static strong ref) until the fresh process ends.
                // A pending close THROWS so Host.StopProducerIfAny keeps the producer — the owner
                // reference being non-null is by-design retention, NOT a failed Stop.
                if (_hostedBackendSelected)
                {
                    StopHostedCore();
                    // Final telemetry flushes only run AFTER a verified hosted close — a pending
                    // throw above never marks the windows final. Flushes follow the EXISTING GPU
                    // telemetry plumbing; QPC helper durations stay CPU-side timings.
                    try { WriteAsyncWindow(Stopwatch.GetTimestamp(), true); }
                    catch (Exception tex)
                    {
                        _host.Log("VC_ASYNC_TELEMETRY_FAIL " + tex.GetType().Name + ": " + tex.Message);
                    }
                    try { WriteGpuWindow(Stopwatch.GetTimestamp(), true); }
                    catch (Exception gex)
                    {
                        _host.Log("VC_GPU_TELEMETRY_FAIL " + gex.GetType().Name + ": " + gex.Message);
                    }
                    return;
                }

                Exception workerError = null;
                var worker = _worker;
                _worker = null;
                foreach (var w in WorkersToStop(worker))
                {
                    bool joined = false;
                    try { joined = w.Stop(500); }
                    catch (Exception wex)
                    {
                        joined = false;
                        workerError = Combine(workerError, wex);
                        _host.Log("VC_WORKER_STOP_FAIL " + wex.GetType().Name + ": " + wex.Message);
                    }
                    if (joined)
                    {
                        if (ReferenceEquals(w, _unjoinedWorker)) _unjoinedWorker = null;
                    }
                    else
                    {
                        _unjoinedWorker = w;   // RETAINED for the next retry — never Abort, never claim release
                        _stopStatus = "worker_stop_timeout_retained";
                        workerError = Combine(workerError, new TimeoutException(
                            "StereoResampleWorker did not exit within 500 ms — thread still alive, worker retained for retry"));
                        _host.Log("VC_WORKER_STOP_TIMEOUT worker retained for retry");
                    }
                }

                // ---- owner restore: ALWAYS attempted, even when workerError != null ----
                Exception ownerError = null;
                var owner = _nativeOwner;
                if (owner != null)
                {
                    bool verified = false;
                    try
                    {
                        verified = owner.Restore();
                        if (!verified || !owner.RestoreVerified)
                            throw new InvalidOperationException(
                                "NativeCaptureOwner.Restore unverified — native recording/render state may still be modified (see VC_NATIVE_RESTORE/VC_RESTORE_FAIL log)");
                        owner.Dispose();
                        _nativeOwner = null;   // owner cleared ONLY after verified restore + successful dispose
                        _host.Log("VC_NATIVE_RESTORE verified=true restoreVerified=true" +
                            " captureCount=" + owner.CaptureCount +
                            " diag=" + owner.DiagnosticText);
                    }
                    catch (Exception ex)
                    {
                        // Failure LOGS AND PROPAGATES — never a misleading "stopped+restored".
                        // _nativeOwner is RETAINED so the next exit attempt can retry restore/dispose.
                        string diag;
                        try { diag = " verified=" + (verified ? "true" : "false") +
                            " restoreVerified=" + (owner.RestoreVerified ? "true" : "false") +
                            " captureCount=" + owner.CaptureCount +
                            " diag=" + owner.DiagnosticText; }
                        catch { diag = " diag_unreadable"; }
                        ownerError = ex;
                        _host.Log("VC_RESTORE_FAIL " + ex.GetType().Name + ": " + ex.Message + diag);
                    }
                }

                if (ownerError != null)
                    _stopStatus = workerError != null
                        ? "worker_timeout_owner_restore_failed" : "owner_restore_failed";
                // Final telemetry flush runs AFTER both cleanup attempts: it must never skip
                // cleanup nor hide a cleanup failure — failures are logged here and the original
                // worker/owner exceptions below propagate unchanged.
                try { WriteAsyncWindow(Stopwatch.GetTimestamp(), true); }
                catch (Exception tex)
                {
                    _host.Log("VC_ASYNC_TELEMETRY_FAIL " + tex.GetType().Name + ": " + tex.Message);
                }
                // GPU final flush runs at the SAME point: after the owner restore AND the CPU
                // worker cleanup were both ATTEMPTED — telemetry must never skip or delay the
                // restore, and a GPU session has no worker (the worker loop is a no-op there).
                try { WriteGpuWindow(Stopwatch.GetTimestamp(), true); }
                catch (Exception gex)
                {
                    _host.Log("VC_GPU_TELEMETRY_FAIL " + gex.GetType().Name + ": " + gex.Message);
                }

                if (workerError != null && ownerError != null)
                    throw new AggregateException(
                        "VC_PRODUCER stop: worker stop AND owner restore both failed — unverified state retained for retry (see inner exceptions)",
                        workerError, ownerError);
                if (ownerError != null) throw ownerError;
                if (workerError != null) throw workerError;
            }));
        }

        /// <summary>Kernel-pulse opt-in — UI-thread-only ASYNC stop request. No-op without a pulse
        /// or when a stop task already exists (StopAsync's own idempotence plus the cached task).
        /// The observer is started ONLY after the task is cached; any exception is stored in
        /// _kernelPulseStopError and every pulse/task/owner ref stays RETAINED — NO native/SDK call
        /// here. Also used by the root QA invalid-runtime monitor to close the scheduler.</summary>
        internal void RequestKernelPulseStop()
        {
            var pulse = _kernelPulse;
            if (pulse == null) return;
            if (_kernelPulseStopTask != null) return;
            try
            {
                var task = pulse.StopAsync();
                if (task == null)
                    throw new InvalidOperationException("KernelUiPulse.StopAsync returned a null task");
                _kernelPulseStopTask = task;   // cached BEFORE the observer starts
                ObserveKernelPulseStopAsync(pulse, task);
            }
            catch (Exception ex)
            {
                // Refs (pulse/owner) are kept for the host 100 ms StopAndRestore retry; the missing
                // task lets the next retry re-attempt StopAsync (idempotent same-task API).
                _kernelPulseStopError = ex.GetType().Name + ": " + ex.Message;
                _host.Log("VC_PRODUCER kernel_pulse_stop_request_fail " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        /// <summary>Kernel-pulse stop observer. Awaits the stop task with NO UI/thread affinity and
        /// NEVER reads owner/WPF state/host/native or uses .Result/.Wait/GetAwaiter/Join — it only
        /// posts the result back to the UI. Publication happens ONLY while the SAME pulse and the
        /// SAME stop task are still current; a post failure keeps every ref so no stop is ever
        /// faked.</summary>
        private async System.Threading.Tasks.Task ObserveKernelPulseStopAsync(
            KernelUiPulse pulse, System.Threading.Tasks.Task<KernelUiPulseStopResult> task)
        {
            KernelUiPulseStopResult result = null;
            string error = null;
            try
            {
                result = await task.ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                error = ex.GetType().Name + ": " + ex.Message;
            }
            try
            {
                _disp.BeginInvoke(DispatcherPriority.Send, (System.Action)(() =>
                {
                    // current-pulse AND current-task check — stale publications are dropped
                    if (!ReferenceEquals(_kernelPulse, pulse) || !ReferenceEquals(_kernelPulseStopTask, task))
                        return;
                    _kernelPulseStopResult = result;
                    _kernelPulseStopError = error;
                }));
            }
            catch (Exception pex)
            {
                // Post failed: refs stay, StopAndRestore keeps throwing kernel_stop_pending —
                // an unpublished stop is never treated as verified.
                _kernelPulseStopError = (error ?? string.Empty) +
                    (string.IsNullOrEmpty(error) ? string.Empty : " | ") +
                    "publish_post_failed " + pex.GetType().Name + ": " + pex.Message;
            }
        }

        /// <summary>Kernel-pulse UI Action — the ONLY thing the pulse drives. Pure managed gates
        /// first (running / generation == hostedStart == original / same owner reference / owner
        /// BoundGeneration / AdmissionOpen / no Tick in progress): a lost generation or an
        /// entered Stop returns forever — an old callback can NEVER Tick or revive a stopped
        /// session. Then the frozen XR session epoch is re-verified against the UI-only
        /// ReadGpuPipelineStats (never callable from the kernel thread); a mismatch or a zero
        /// epoch converges via RecordHostedCaptureFailure("kernel_pulse_epoch_invalid"), no
        /// fallback. Only then the EXISTING Tick runs unchanged — no extra pose reads, no camera
        /// mutation, no SDK thread, no Tick math change. While the owner is still PENDING this
        /// same Tick completes the original Ready/calibration path (no live-ready gate blocks
        /// startup).</summary>
        private void OnKernelPulse(int originalGeneration, NC04.NC01HostedSceneOwner originalOwner, ulong originalXrEpoch)
        {
            if (!_running || _generation != originalGeneration || _hostedStartGeneration != originalGeneration)
                return;
            if (!ReferenceEquals(_hostedOwner, originalOwner)) return;
            if (_tickInProgress) return;

            string ownerGateFailure = null;
            try
            {
                if (originalOwner == null)
                    ownerGateFailure = "kernel_pulse_owner_missing";
                else if (originalOwner.BoundGeneration != originalGeneration)
                    ownerGateFailure = "kernel_pulse_bound_generation_mismatch";
                else if (!originalOwner.AdmissionOpen)
                    ownerGateFailure = "kernel_pulse_admission_closed";
            }
            catch (Exception ex)
            {
                ownerGateFailure = "kernel_pulse_owner_gate_exception " + ex.GetType().Name + ": " + ex.Message;
            }
            if (ownerGateFailure != null)
            {
                if (_running && _generation == originalGeneration && _hostedStartGeneration == originalGeneration &&
                    ReferenceEquals(_hostedOwner, originalOwner))
                {
                    try
                    {
                        RecordHostedCaptureFailure(originalGeneration, ownerGateFailure);
                    }
                    catch { }
                }
                return;
            }

            bool epochOk = false;
            try
            {
                var stats = _host.ReadGpuPipelineStats();
                if (stats != null)
                {
                    var epoch = stats.Value.sessionEpoch;
                    epochOk = epoch != 0UL && epoch == originalXrEpoch;
                }
            }
            catch { }
            if (!epochOk)
            {
                // Honest convergence — the pulse route never falls back to a timer or a stale epoch.
                RecordHostedCaptureFailure(originalGeneration, "kernel_pulse_epoch_invalid");
                return;
            }

            try
            {
                Tick();
            }
            catch (Exception ex)
            {
                if (_running && _generation == originalGeneration && _hostedStartGeneration == originalGeneration &&
                    ReferenceEquals(_hostedOwner, originalOwner))
                {
                    try
                    {
                        RecordHostedCaptureFailure(originalGeneration,
                            "kernel_pulse_tick_failure " + ex.GetType().Name + ": " + ex.Message);
                    }
                    catch { }
                }
            }
        }

        /// <summary>Hosted stop core — PURE managed gate close + honest completion probe only. Runs
        /// inside the StopAndRestore dispatcher block AFTER timer stop → running=false → generation++
        /// have already invalidated the generation. The owner's OWN BoundGeneration is saved at the
        /// FIRST Stop entry and reused by every retry — repeated Stops can never misapply the
        /// incremented producer generation. Verified close: _stopStatus stays "stopped", the owner
        /// reference is RETAINED (no Restore/Dispose/unload). Busy: throws an explicit pending
        /// exception so the producer AND owner are kept for a later retry; NO absolute deadline is
        /// claimed against a blocking GetRT/SurfaceRender. A null field never fakes StopVerified.</summary>
        private void StopHostedCore()
        {
            ClearHostedPreviewCaches();
            _hostedFailureNotifyAllowed = false;   // explicit Stop cancels any unsent failure notification
            var ho = _hostedOwner;
            if (ho == null) return;   // never hosted past the factory — nothing to close
            if (_hostedStopBoundGeneration < 0) _hostedStopBoundGeneration = ho.BoundGeneration;
            // OBS-SHARED (Card3B1): a shared stop NEVER returns through the generic verified path
            // below — no ho.StopVerified shortcut, no generic owner-only completion, no native
            // finalize from here. It converges ONLY via the finite source steps + host-confirmed
            // native destroy (ConfirmObsSharedNativeDestroyed).
            if (_obsSharedRequested && _obsSharedStopEntered)
            {
                StopHostedCoreObsShared(ho);
                return;   // reached only when the whole stop is already verified — every other outcome throws pending/terminal
            }
            int stopGen = _hostedStopBoundGeneration;
            if (stopGen <= 0)
            {
                _host.Log("VC_PRODUCER hosted_stop_no_bound_generation ownerBound=" + ho.BoundGeneration.ToString(CultureInfo.InvariantCulture) +
                    " — no BeginHosting ever bound this lease, nothing to close");
                return;
            }
            try { ho.RequestStop(stopGen); }
            catch (Exception rex)
            {
                _host.Log("VC_PRODUCER hosted_stop_request_fail " + rex.GetType().Name + ": " + rex.Message);
            }
            // NC02-XR-P3: with staging selected, close the producer via FINITE UI-thread drain
            // steps (owner-owned native Stop/Destroy only, at most ONE step per Stop turn).
            // Tick-in-progress or CaptureBusy keeps the honest pending/throw — never drained here.
            bool staged = _nc02StagingRequested;
            bool drainStepOk = false;
            if (staged)
            {
                if (_nc02DrainBudgetExpired)
                {
                    _host.Log("VC_PRODUCER nc02_drain_budget_expired generation=" + stopGen.ToString(CultureInfo.InvariantCulture) +
                        " attempts=" + _nc02DrainAttempts.ToString(CultureInfo.InvariantCulture) +
                        " ownerRetained=true root_fail_no_native_retry — staged stop stays unclosed; no automatic new-session retry");
                }
                else if (_tickInProgress || ho.CaptureBusy)
                {
                    _host.Log("VC_PRODUCER nc02_drain_deferred captureBusy=" + (ho.CaptureBusy ? "true" : "false") +
                        " tickInProgress=" + (_tickInProgress ? "true" : "false") +
                        " — pending preserved, no drain while an SDK transaction is entered");
                }
                else
                {
                    string drainReason;
                    drainStepOk = TryStepNc02StagingStop(ho, stopGen, out drainReason);
                    if (!drainStepOk)
                        _host.Log("VC_PRODUCER nc02_drain_step_false generation=" + stopGen.ToString(CultureInfo.InvariantCulture) +
                            " reason=" + (drainReason ?? "null") +
                            " attempts=" + _nc02DrainAttempts.ToString(CultureInfo.InvariantCulture) +
                            " budgetExpired=" + (_nc02DrainBudgetExpired ? "true" : "false"));
                }
            }
            bool verified;
            if (staged && !drainStepOk)
            {
                verified = false;   // no proven step this turn — stay pending, keep rows/maps/owner
            }
            else
            {
                try
                {
                    verified = ho.StopVerified || ho.TryCompleteStop(stopGen);
                    if (staged && verified &&
                        (_tickInProgress || !ho.StagingHelperDrained || ho.StagingPendingCount != 0))
                        verified = false;   // helper not provably closed — stay pending
                }
                catch (Exception vex)
                {
                    verified = false;
                    _host.Log("VC_PRODUCER hosted_stop_verify_fail " + vex.GetType().Name + ": " + vex.Message);
                }
            }
            if (verified)
            {
                if (staged)
                {
                    _nc02CaptureRows.Clear();
                    Array.Clear(_nc02MapPairIds, 0, _nc02MapPairIds.Length);
                    Array.Clear(_nc02MapFrameSerials, 0, _nc02MapFrameSerials.Length);
                    Array.Clear(_nc02PollCounts, 0, _nc02PollCounts.Length);
                }
                _hostedStopPending = false;
                _stopStatus = "stopped";
                _host.Log("VC_PRODUCER hosted_stop_verified generation=" + stopGen.ToString(CultureInfo.InvariantCulture) +
                    " ownerRetainedByDesign=true no_legacy_restore no_sdk_mutation no_helper_unload" +
                    (staged ? " nc02_staging_rows_maps_cleared=true" : ""));
                return;
            }
            // HONEST PENDING — an entered Begin/Ready/raw-pair/unlock SDK transaction has not exited.
            _hostedStopPending = true;
            _stopStatus = "hosted_stop_pending_capture_busy";
            _host.Log("VC_PRODUCER hosted_stop_pending generation=" + stopGen.ToString(CultureInfo.InvariantCulture) +
                " captureBusy=" + (ho.CaptureBusy ? "true" : "false") +
                " tickInProgress=" + (_tickInProgress ? "true" : "false") +
                " ownerRetained=true — retry StopAndRestore after the entered transaction exits (root harness: wait for busy/tick exit; no XR absolute deadline promised)");
            throw new InvalidOperationException(
                "VC_PRODUCER hosted stop pending — an entered SDK transaction is still active (generation=" +
                stopGen.ToString(CultureInfo.InvariantCulture) + ", captureBusy=" +
                (ho.CaptureBusy ? "true" : "false") +
                "). Producer and hosted owner RETAINED; retry StopAndRestore after the tick/capture exits.");
        }

        /// <summary>NC02-XR-P3: ONE finite UI-thread staging-stop drain step. Validates
        /// same-owner / original bound generation / UI access / no tick / no owner capture-busy
        /// BEFORE calling the owner's own TryDrainStagingStop (which runs at most one native
        /// StopStep — no sleep, no loop, no direct nativeStop/Destroy here). Budget: max 32
        /// attempts or 500 ms of Stopwatch ticks since the FIRST actual step — delayed host
        /// retries are honored, but no hard driver deadline is promised. Over budget sets
        /// _nc02DrainBudgetExpired permanently and always returns false; owner and refs stay
        /// retained for the root failure verdict. Pending/exception/false never clear rows/maps.</summary>
        private bool TryStepNc02StagingStop(NC04.NC01HostedSceneOwner owner, int originalGeneration, out string reason)
        {
            reason = null;
            if (owner == null || !ReferenceEquals(_hostedOwner, owner))
            {
                reason = "foreign_owner";
                return false;
            }
            if (originalGeneration <= 0 || _hostedStopBoundGeneration != originalGeneration ||
                owner.BoundGeneration != originalGeneration)
            {
                reason = "generation_mismatch";
                return false;
            }
            if (_disp == null || !_disp.CheckAccess())
            {
                reason = "ui_thread_required";
                return false;
            }
            if (_tickInProgress)
            {
                reason = "tick_in_progress";
                return false;
            }
            if (owner.CaptureBusy)
            {
                reason = "capture_busy";
                return false;
            }
            if (_nc02DrainBudgetExpired)
            {
                reason = "drain_budget_expired";
                return false;
            }
            if (_nc02DrainAttempts == 0)
                _nc02DrainBeginQpc = System.Diagnostics.Stopwatch.GetTimestamp();
            _nc02DrainAttempts = checked(_nc02DrainAttempts + 1);
            long elapsedQpc = System.Diagnostics.Stopwatch.GetTimestamp() - _nc02DrainBeginQpc;
            if (_nc02DrainAttempts > 32 || elapsedQpc > System.Diagnostics.Stopwatch.Frequency / 2)
            {
                _nc02DrainBudgetExpired = true;
                reason = "drain_budget_expired";
                _host.Log("VC_PRODUCER nc02_drain_budget_set generation=" + originalGeneration.ToString(CultureInfo.InvariantCulture) +
                    " attempts=" + _nc02DrainAttempts.ToString(CultureInfo.InvariantCulture) +
                    " elapsedQpc=" + elapsedQpc.ToString(CultureInfo.InvariantCulture) +
                    " frequency=" + System.Diagnostics.Stopwatch.Frequency.ToString(CultureInfo.InvariantCulture) +
                    " — owner retained, further steps refuse forever");
                return false;
            }
            bool step;
            try
            {
                step = owner.TryDrainStagingStop(originalGeneration, out reason);
            }
            catch (Exception ex)
            {
                reason = ex.GetType().Name + ": " + ex.Message;
                return false;
            }
            if (!ReferenceEquals(_hostedOwner, owner) || owner.BoundGeneration != originalGeneration)
            {
                reason = "post_step_owner_or_generation_changed";
                return false;
            }
            return step;
        }

        // ============================================================================================
        // OBS-SHARED producer SOURCE stop (Card3B1 — managed half only). The two Owner stops are pure
        // managed and release NO D9 device / official Window / native resource, so RequestStop on the
        // frozen original generation is immediate and correct; the native bridge drain is requested
        // EXACTLY ONCE per original stop (metadata only — no UI GPU work). One finite SOURCE-UI step
        // runs at most THREE ledger passes + ONE cached Stop/BridgeStats read pair + ONE eligible
        // SourceUI finalize — no Sleep/Join/Wait/DispatcherFrame/PushFrame/Poll/End/Flush and no SDK
        // read. Real context exit/cleanup and XRDestroyInstance belong to the native worker; the
        // future 3B2 host 100 ms deferred timer calls TryStepObsSharedSourceStop, then confirms the
        // WHOLE stop through ConfirmObsSharedNativeDestroyed after the actual native destroy.
        // ============================================================================================

        /// <summary>QA/host: OBS-SHARED selection FROZEN at construction.</summary>
        internal bool ObsSharedSelected { get { return _obsSharedRequested; } }
        /// <summary>QA/host: the shared stop outcome is TERMINAL (failure/unknown/quarantine/
        /// false-finalize/budget/exception). The host MUST stop automatic retries; refs, packets and
        /// the combined error stay retained and Enter stays blocked — this can never reopen here.</summary>
        internal bool ObsSharedStopTerminal { get { return _obsSharedStopTerminal; } }
        /// <summary>QA/host: the shared SOURCE is finalized and ready for native teardown — proven
        /// EITHER by an actual SourceUIFinalize rc==0 plus verified real readback, OR by the distinct
        /// never-bound proof. The WHOLE stop is NOT yet verified while this is true.</summary>
        internal bool ObsSharedSourceReadyForNativeTeardown { get { return _obsSharedSourceReady; } }
        /// <summary>QA/host: the ENTIRE shared cleanup completed — actual destroy rc==0 with native
        /// state OFF(0), original identity/busy/tick-finally all quiet, and the original owner's
        /// TryCompleteStop(originalBoundGeneration) returned true.</summary>
        internal bool ObsSharedWholeStopVerified { get { return _obsSharedWholeStopVerified; } }
        /// <summary>QA/host: DISTINCT never-bound source-ready flag — a canceled Start that NEVER
        /// prepared. Kept separate because FinalizeUnavailable(-2) may never be treated as a real
        /// SourceUIFinalize rc==0; the host and QA must be able to tell the two apart.</summary>
        internal bool ObsSharedNeverBoundSourceReady { get { return _obsSharedNeverBoundSourceReady; } }
        /// <summary>QA: the ACTUAL verified Stop32 snapshot, stored ONLY after a real SourceUIFinalize
        /// rc==0 whose fresh readback reported flags==7 / pending==0 / the ORIGINAL epoch+generation.
        /// Never synthesized. The later native XRDestroyInstance clears the native stop32, so host QA
        /// relies on THIS real snapshot.</summary>
        internal VRSIMPlugin.SharedGpuStopStateV1 ObsSharedFinalStopState { get { return _obsSharedFinalStopState; } }
        /// <summary>True only when ObsSharedFinalStopState holds the real post-finalize readback.</summary>
        internal bool ObsSharedFinalStopStateValid { get { return _obsSharedFinalStopStateValid; } }
        /// <summary>QA/host: the frozen ORIGINAL stop identity — BoundGeneration (−1 = never bound)
        /// and the shared bound epoch resolved from retained tickets / actual Stop32 only.</summary>
        internal int ObsSharedStopBoundGeneration { get { return _obsSharedStopBoundGeneration; } }
        /// <summary>QA/host: the ORIGINAL shared bound epoch frozen at the first Stop entry.</summary>
        internal ulong ObsSharedStopBoundEpoch { get { return _obsSharedStopBoundEpoch; } }
        /// <summary>QA/host: ACTUAL one-shot RequestSharedGpuDrain rc (int.MinValue = exception or
        /// never attempted). Metadata-only request; a nonzero rc is retained evidence, never inferred.</summary>
        internal int ObsSharedDrainRequestRc { get { return _obsSharedDrainRequestRc; } }
        /// <summary>QA/host: finite SOURCE-UI steps actually taken for this stop (budget 32 / 5000 ms).</summary>
        internal int ObsSharedSourceStopSteps { get { return _obsSharedSourceStopSteps; } }
        /// <summary>QA/host: the retained combined shared-stop failure evidence (actual rc faults +
        /// exceptions, inner stacks intact); null when none. Separate from the 3A capture fault, which
        /// is never overwritten or dropped by the stop path.</summary>
        internal Exception ObsSharedStopFailureEvidence { get { return _obsSharedStopError; } }

        /// <summary>OBS-SHARED (Card3B1) first-Stop/first-failure freeze — PURE managed plus cached
        /// native-metadata reads ONLY (no Discard, no Finalize, no Confirm, no owner/native mutation).
        /// ONE-shot per original stop and MUST run BEFORE any _generation++ invalidation: freezes the
        /// exact original hosted owner, the POSITIVE owner BoundGeneration (falling back to the exact
        /// Start original _hostedStartGeneration while the owner is still 0), the old-ticket/ACTUAL
        /// Stop32 bound epoch and records firstStopQPC once. Never rewrites an old ticket, generation
        /// or epoch afterwards; the retained _obsSharedLastFault and every packet/ref stay untouched.</summary>
        private void FreezeObsSharedFirstStopOnce()
        {
            if (!_obsSharedRequested || _obsSharedStopEntered) return;
            _obsSharedStopEntered = true;
            _obsSharedFirstStopQpc = Stopwatch.GetTimestamp();   // recorded exactly once
            var fho = _hostedOwner;
            _obsSharedStopOwner = fho;
            int frozenBound = 0;
            if (fho != null)
            {
                try { frozenBound = fho.BoundGeneration; } catch { frozenBound = -1; }
            }
            if (frozenBound > 0) _obsSharedStopBoundGeneration = frozenBound;
            else if (_hostedStartGeneration > 0)
                _obsSharedStopBoundGeneration = _hostedStartGeneration;   // owner still 0 — exact Start original generation
            if (_obsSharedStopBoundGeneration > 0 && _hostedStopBoundGeneration < 0)
                _hostedStopBoundGeneration = _obsSharedStopBoundGeneration;
            ulong frozenEpoch = 0UL;
            bool epochFrom = false, epochConflict = false;
            for (int i = 0; i < _obsSharedTickets.Length; i++)
            {
                var tk = _obsSharedTickets[i];
                if (tk.ticket == 0UL) continue;
                if (!epochFrom) { frozenEpoch = tk.epoch; epochFrom = true; }
                else if (frozenEpoch != tk.epoch) epochConflict = true;
            }
            string epochSource;
            if (epochFrom && !epochConflict)
            {
                _obsSharedStopBoundEpoch = frozenEpoch;
                epochSource = "retained_ticket";
            }
            else
            {
                VRSIMPlugin.SharedGpuStopStateV1 fstop;
                int frc;
                try { frc = _host.ReadSharedGpuStop(out fstop); }
                catch { fstop = default(VRSIMPlugin.SharedGpuStopStateV1); frc = -1; }
                if (frc == (int)VRSIMPlugin.SharedGpuResultV1.Completed &&
                    (_obsSharedStopBoundGeneration <= 0 ||
                     (fstop.boundGeneration == (uint)_obsSharedStopBoundGeneration && fstop.boundEpoch != 0UL)))
                { _obsSharedStopBoundEpoch = fstop.boundEpoch; epochSource = "actual_stop32"; }
                else if (epochConflict)
                { _obsSharedStopBoundEpoch = 0UL; epochSource = "conflict_zeroed_never_bound_forbidden"; }
                else
                { _obsSharedStopBoundEpoch = 0UL; epochSource = "none_zero"; }
            }
            _host.Log("VC_PRODUCER obs_shared_stop_frozen boundGeneration=" +
                _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture) +
                " boundEpoch=" + _obsSharedStopBoundEpoch.ToString(CultureInfo.InvariantCulture) +
                " epochSource=" + epochSource + " freeze_before_generation_increment=true");
        }

        /// <summary>OBS-SHARED (Card3B1) same-call admission close — pure managed owner RequestStop plus
        /// the metadata-only native drain RequestSharedGpuDrain, executed EXACTLY ONCE per original
        /// stop immediately inside the same Stop / capture-failure call, AFTER the generation
        /// invalidation and BEFORE any new submit could be attempted. RequestStop targets the frozen
        /// POSITIVE original generation; a pre-Begin RequestStop binds/closes the owner and the
        /// resulting positive BoundGeneration is verified (nonpositive → honest terminal, all refs
        /// retained). No Discard/Finalize/Complete/Confirm happens here; actual rc/exception is saved
        /// into the retained stop evidence only.</summary>
        private void CloseObsSharedFirstStopAdmissionOnce()
        {
            if (!_obsSharedRequested || !_obsSharedStopEntered) return;
            if (_obsSharedDrainAttempted) return;
            _obsSharedDrainAttempted = true;
            var so = _obsSharedStopOwner;
            if (so != null)
            {
                if (_obsSharedStopBoundGeneration > 0)
                {
                    try { so.RequestStop(_obsSharedStopBoundGeneration); }
                    catch (Exception rex)
                    {
                        _obsSharedStopError = Combine(_obsSharedStopError, rex);
                        _host.Log("VC_PRODUCER obs_shared_stop_request_fail " +
                            rex.GetType().Name + ": " + rex.Message);
                    }
                    // Pre-Begin RequestStop binds/closes — verify the RESULTING BoundGeneration EQUALS
                    // the frozen POSITIVE original generation EXACTLY (not merely positive) AND the
                    // PURE managed admission gate is closed. Anything else (changed gen, read
                    // exception, admission still open or read exception) is an honest terminal:
                    // every ref/packet/error stays RETAINED and sourceReady can never be reached.
                    int postBound;
                    try { postBound = so.BoundGeneration; } catch { postBound = -1; }
                    if (postBound != _obsSharedStopBoundGeneration)
                        MarkObsSharedStopTerminal("request_stop_bind_verify_not_original_generation");
                    else
                    {
                        bool admissionOpen = true;   // unknown/exception → conservatively still-open → terminal
                        try { admissionOpen = so.AdmissionOpen; }
                        catch (Exception aex)
                        {
                            _obsSharedStopError = Combine(_obsSharedStopError, aex);
                        }
                        if (admissionOpen)
                            MarkObsSharedStopTerminal("request_stop_admission_still_open_no_source_ready");
                    }
                }
                else
                {
                    int curBound;
                    try { curBound = so.BoundGeneration; } catch { curBound = -1; }
                    _host.Log("VC_PRODUCER obs_shared_stop_no_bound_generation ownerBound=" +
                        curBound.ToString(CultureInfo.InvariantCulture) +
                        " — no positive frozen generation; gen0 can never become sourceReady, the step path goes terminal retained");
                }
            }
            // kernel-render-120 shared-stop integration: on the SAME first shared stop/failure call,
            // after the managed admission close above and BEFORE the original native drain request,
            // NONBLOCKING-request the pulse stop (StopAsync only — never Wait/Join/Task.Result) and
            // freeze the EXACT resulting pulse + task references once. A StopAsync throw keeps the
            // real _kernelPulseStopError and the quiet helper marks terminal; a plain shared timer
            // has no pulse here and freezes null refs. The frozen pulse is never substituted.
            RequestKernelPulseStop();
            _obsSharedStopPulse = _kernelPulse;
            _obsSharedStopPulseTask = _kernelPulseStopTask;
            try { _obsSharedDrainRequestRc = _host.RequestSharedGpuDrain(); }
            catch (Exception dex)
            {
                _obsSharedDrainRequestRc = int.MinValue;
                _obsSharedStopError = Combine(_obsSharedStopError, dex);
                _host.Log("VC_PRODUCER obs_shared_drain_request_fail " +
                    dex.GetType().Name + ": " + dex.Message);
            }
            _host.Log("VC_PRODUCER obs_shared_drain_requested rc=" +
                _obsSharedDrainRequestRc.ToString(CultureInfo.InvariantCulture) +
                " metadata_only_no_ui_gpu once_per_original_stop=true");
        }

        /// <summary>Pure-managed kernel-scheduler quiet proof for the OBS-SHARED stop steps — NO
        /// SDK/native/GPU call and no wait/join/Task.Result; only frozen references plus the
        /// observer-published immutable stop result. true = the scheduler is provably quiet (plain
        /// shared timer with no pulse, an honest neverCreated, or a fully verified stop).
        /// false + terminal=true = lost/forged/errored/unverified proof: every ref stays retained
        /// and the shared stop converges to terminal forever. false + terminal=false = HONEST
        /// PENDING bounded by the unchanged first-stop 5000 ms deadline — a pending stop is never
        /// bypassed and the frozen pulse is never re-created or substituted.</summary>
        private bool ObsSharedKernelQuietVerified(out string reason, out bool terminal)
        {
            reason = null;
            terminal = false;
            var frozen = _obsSharedStopPulse;
            if (!_kernelRender120Requested)
            {
                if (frozen == null && _kernelPulse == null && !_kernelRender120PulseEverCreated)
                    return true;   // plain shared timer — no scheduler to quiet
                reason = "shared_plain_timer_foreign_kernel_pulse_ref";
                terminal = true;
                return false;
            }
            if (_kernelRender120PulseEverCreated)
            {
                if (frozen == null || !ReferenceEquals(_kernelPulse, frozen))
                {
                    reason = "kernel_render120_pulse_ref_lost_or_substituted";
                    terminal = true;
                    return false;
                }
                if (!string.IsNullOrEmpty(_kernelPulseStopError))
                {
                    reason = "kernel_render120_stop_request_error " + _kernelPulseStopError;
                    terminal = true;
                    return false;
                }
                var frozenTask = _obsSharedStopPulseTask;
                if (frozenTask == null || !ReferenceEquals(_kernelPulseStopTask, frozenTask))
                {
                    reason = "kernel_render120_stop_task_pending";
                    return false;
                }
                var res = _kernelPulseStopResult;
                if (res == null)
                {
                    reason = "kernel_render120_stop_result_pending";
                    return false;
                }
                if (res.Failure != null || !res.StopVerified || !res.WorkerExited || !res.HandlesReleased ||
                    res.CallbackPending || res.CallbackExecuting || res.TimedOut)
                {
                    reason = "kernel_render120_stop_unverified stopVerified=" + (res.StopVerified ? "true" : "false") +
                        " workerExited=" + (res.WorkerExited ? "true" : "false") +
                        " handlesReleased=" + (res.HandlesReleased ? "true" : "false") +
                        " callbackPending=" + (res.CallbackPending ? "true" : "false") +
                        " callbackExecuting=" + (res.CallbackExecuting ? "true" : "false") +
                        " timedOut=" + (res.TimedOut ? "true" : "false") +
                        " failure=" + (res.Failure == null ? "none" : res.Failure);
                    terminal = true;
                    return false;
                }
                return true;
            }
            // New-mode cancellation STRICTLY BEFORE any pulse creation: honestly neverCreated only
            // when every pulse/task/result/error field and BOTH frozen refs are absent — a
            // successful stop result is never synthesized for a pulse that never existed.
            if (_kernelPulse == null && _kernelPulseStopTask == null && _kernelPulseStopResult == null &&
                string.IsNullOrEmpty(_kernelPulseStopError) && frozen == null && _obsSharedStopPulseTask == null)
            {
                reason = "kernel_render120_neverCreated_no_pulse";
                return true;
            }
            reason = "kernel_render120_neverCreated_proof_inconsistent";
            terminal = true;
            return false;
        }

        /// <summary>READ-ONLY QA: the exact KernelUiPulse reference frozen at the first shared stop
        /// (null = plain shared timer or an honest neverCreated).</summary>
        internal KernelUiPulse ObsSharedStopPulseForMonitor { get { return _obsSharedStopPulse; } }

        /// <summary>READ-ONLY QA: the observer-published immutable kernel stop result for the frozen
        /// shared pulse (null = not yet published — honest pending, never a verified stop).</summary>
        internal KernelUiPulseStopResult ObsSharedKernelStopResultForMonitor { get { return _kernelPulseStopResult; } }

        /// <summary>ONE finite shared SOURCE-UI stop step (host deferred timer OR a StopAndRestore
        /// retry — UI dispatcher only). false = pending OR terminal; true = the SOURCE finalized and
        /// is ready for native teardown while the WHOLE cleanup is NOT yet complete. Never
        /// Discards/Finalizes/Completes while a tick, an owner capture or the whole Tick finally is
        /// still on the stack — it only leaves the honest pending state in that case.</summary>
        internal bool TryStepObsSharedSourceStop(out string reason)
        {
            reason = null;
            if (!_obsSharedRequested) { reason = "obs_shared_not_selected"; return false; }
            if (!_obsSharedStopEntered) { reason = "shared_stop_not_frozen"; return false; }
            if (_obsSharedWholeStopVerified) { reason = "whole_stop_already_verified"; return false; }
            if (_obsSharedStopTerminal) { reason = "terminal_retained"; return false; }
            if (_obsSharedSourceReady)
            {
                // kernel-render-120: sourceReady is only EVER reachable through an already-gated
                // quiet-proven path and the proof is immutable — re-gate the shortcut IN PLACE so a
                // never-yet-quiet scheduler can never ride it (pending stays behind the unchanged
                // first-stop 5000 ms deadline below).
                bool readySchedTerminal;
                string readySchedReason;
                if (ObsSharedKernelQuietVerified(out readySchedReason, out readySchedTerminal))
                { reason = "source_ready_awaiting_native_destroy_confirm"; return true; }
                if (readySchedTerminal)
                { MarkObsSharedStopTerminal("scheduler_quiet_terminal " + readySchedReason); return false; }
                reason = "scheduler_quiet_pending " + readySchedReason;
                return false;
            }
            if (_disp == null || !_disp.CheckAccess()) { reason = "ui_thread_required"; return false; }
            // Card3B1: the 5000 ms monotonic deadline runs from the FIRST stop/failure freeze and is
            // evaluated BEFORE the tick/finally/ownerBusy early returns — a busy owner can never keep
            // resetting (starving) the budget. Marking terminal only records the state; it does NOT
            // promise to interrupt a blocking SDK/GPU call. steps<=32 still applies below.
            if (_obsSharedFirstStopQpc != 0L &&
                Stopwatch.GetTimestamp() - _obsSharedFirstStopQpc > (long)Stopwatch.Frequency * 5L)
            { MarkObsSharedStopTerminal("first_stop_deadline_expired_5000ms"); return false; }
            if (_tickInProgress || _obsSharedFinallyActive) { reason = "tick_or_finally_on_stack"; return false; }
            var ho = _obsSharedStopOwner;
            if (ho == null || !ReferenceEquals(_hostedOwner, ho))
            { MarkObsSharedStopTerminal("original_owner_lost"); return false; }
            if (_obsSharedStopBoundGeneration != ho.BoundGeneration)
            { MarkObsSharedStopTerminal("owner_bound_generation_changed"); return false; }
            // A frozen gen0 (no positive original bound generation) can NEVER become sourceReady.
            if (_obsSharedStopBoundGeneration <= 0)
            { MarkObsSharedStopTerminal("frozen_original_bound_generation_not_positive_gen0"); return false; }
            if (ho.CaptureBusy) { reason = "owner_capture_busy"; return false; }

            // Budget per ORIGINAL stop: 32 steps OR 5000 ms monotonic QPC. The native worker's own
            // 128-attempt / 1000 ms deadline is earlier; a late success can never be faked here.
            if (_obsSharedSourceStopSteps == 0) _obsSharedSourceStopBeginQpc = Stopwatch.GetTimestamp();
            _obsSharedSourceStopSteps = checked(_obsSharedSourceStopSteps + 1);
            long stepElapsedQpc = Stopwatch.GetTimestamp() - _obsSharedSourceStopBeginQpc;
            if (_obsSharedSourceStopSteps > 32 || stepElapsedQpc > Stopwatch.Frequency * 5)
            { MarkObsSharedStopTerminal("source_stop_step_budget_expired"); return false; }

            // ONE cached Stop32 + ONE cached BridgeStats read for the whole step (cached metadata only).
            VRSIMPlugin.SharedGpuStopStateV1 stop;
            int stopRc;
            try { stopRc = _host.ReadSharedGpuStop(out stop); }
            catch (Exception ex)
            {
                _obsSharedStopError = Combine(_obsSharedStopError, ex);
                MarkObsSharedStopTerminal("stop_read_exception " + ex.GetType().Name);
                return false;
            }
            if (stopRc != (int)VRSIMPlugin.SharedGpuResultV1.Completed)
            {
                if (stopRc == (int)VRSIMPlugin.SharedGpuResultV1.Pending) { reason = "stop_read_pending"; return false; }
                _obsSharedStopError = Combine(_obsSharedStopError, new InvalidOperationException(
                    "OBS_SHARED_STOP_READ_NOT_COMPLETED rc=" + stopRc.ToString(CultureInfo.InvariantCulture)));
                MarkObsSharedStopTerminal("stop_read_unknown_or_quarantine");
                return false;
            }
            VRSIMPlugin.SharedGpuBridgeStatsV1 stats;
            int statsRc;
            try { statsRc = _host.ReadSharedGpuBridgeStats(out stats); }
            catch (Exception ex)
            {
                _obsSharedStopError = Combine(_obsSharedStopError, ex);
                MarkObsSharedStopTerminal("bridge_stats_read_exception " + ex.GetType().Name);
                return false;
            }
            if (statsRc != (int)VRSIMPlugin.SharedGpuResultV1.Completed)
            {
                if (statsRc == (int)VRSIMPlugin.SharedGpuResultV1.Pending) { reason = "bridge_stats_read_pending"; return false; }
                _obsSharedStopError = Combine(_obsSharedStopError, new InvalidOperationException(
                    "OBS_SHARED_BRIDGE_STATS_READ_NOT_COMPLETED rc=" + statsRc.ToString(CultureInfo.InvariantCulture)));
                MarkObsSharedStopTerminal("bridge_stats_unknown_or_quarantine");
                return false;
            }
            if ((stop.flags & ~ObsSharedStopFlagsSourceFinalizedV1) != 0u)
            { MarkObsSharedStopTerminal("stop_flags_unknown_high_bits"); return false; }

            // Independent NEVER-BOUND proof (contract 6) — checked FIRST and NEVER conflated with a
            // real SourceUIFinalize rc==0 (FinalizeUnavailable(-2) is never a substitute either).
            if (ObsSharedLedgerProvenEmpty() &&
                stop.boundEpoch == 0UL && stop.boundGeneration == 0u &&
                stop.pendingSlots == 0u && stop.flags == 0u &&
                stats.initCount == 0u && ObsSharedAllBridgeSlotsFree(stats))
            {
                // kernel-render-120: the never-bound proof may only be ADDED while the scheduler
                // is quiet-proven; pending stays honest, invalid/lost proof is terminal retained.
                bool neverBoundSchedTerminal;
                string neverBoundSchedReason;
                if (!ObsSharedKernelQuietVerified(out neverBoundSchedReason, out neverBoundSchedTerminal))
                {
                    if (neverBoundSchedTerminal)
                    { MarkObsSharedStopTerminal("scheduler_quiet_terminal " + neverBoundSchedReason); return false; }
                    reason = "scheduler_quiet_pending " + neverBoundSchedReason;
                    return false;
                }
                return TryProveObsSharedNeverBoundStep(ho, out reason);
            }

            // ---- at most THREE ledger passes ----
            // Pass 1: exact DISCARD of every locally-owned PREPARED packet (unchanged 3A helper).
            // Only an actual native rc==0 clears an entry; Pending/error RETAINS ticket + identity +
            // owned with the real fault combined into the retained stop evidence.
            for (int i = 0; i < _obsSharedTickets.Length; i++)
            {
                if (!_obsSharedOwned[i]) continue;
                Exception df = TryDiscardObsSharedPrepared(i);
                if (df != null) _obsSharedStopError = Combine(_obsSharedStopError, df);
            }
            // Pass 2: COMMITTED-but-unread packets clear ONLY on original epoch+generation match with
            // the native bridge's cached slot genuinely FREE — a READING/INFLIGHT (any non-FREE) slot
            // is waited on, NEVER UI-cancelled here. Pending evidence retains every packet.
            for (int i = 0; i < _obsSharedTickets.Length; i++)
            {
                if (!_obsSharedCommitted[i] || _obsSharedOwned[i]) continue;
                var tk = _obsSharedTickets[i];
                if (tk.ticket == 0UL) continue;
                if (tk.slotIndex >= ObsSharedSlotCountV1)
                { MarkObsSharedStopTerminal("committed_packet_malformed_slot"); return false; }
                if (stop.boundEpoch != tk.epoch || stop.boundGeneration != tk.generation) continue;
                uint slotState = tk.slotIndex == 0u ? stats.slotState0
                    : (tk.slotIndex == 1u ? stats.slotState1 : stats.slotState2);
                if (slotState != ObsSharedBridgeSlotStateFreeV1) continue;
                // Proven FREE on the ORIGINAL identity — recycle-equivalent managed clear, no synthesis.
                _obsSharedCommitted[i] = false;
                _obsSharedTickets[i] = default(VRSIMPlugin.SharedGpuTicketV1);
                _obsSharedIdentities[i] = default(VRSIMPlugin.SharedGpuCaptureIdentityV1);
            }
            // Pass 3: with the bridge ACTUALLY CLOSED and pendingSlots NONZERO, an exact DISCARD is
            // permitted ONLY for a slot the cached bridge reports as precisely COMMITTED. pendingSlots
            // counts COMMITTED too and is deliberately NOT required to be 0 here. FREE(0) packets are
            // pass 2's proven-slot-FREE clear; PREPARING(1)/PREPARED(2)/READING(4)/INFLIGHT(5) and
            // every unknown value stay worker-owned — the source DISCARD is NEVER called on them, the
            // ticket is retained and the step leaves an honest pending. The old-ticket epoch/generation
            // exact gate still guards against a snapshot race, and ONLY an actual native rc==0 clears
            // the ledger entry. Never force a slot FREE.
            const uint ObsSharedBridgeSlotStateCommittedV1 = 3u;   // frozen SharedStereoWire.h cached enum: COMMITTED=3 (already proven)
            if ((stop.flags & ObsSharedStopFlagsBridgeClosedV1) != 0u && stop.pendingSlots != 0u)
            {
                for (int i = 0; i < _obsSharedTickets.Length; i++)
                {
                    if (!_obsSharedCommitted[i] || _obsSharedOwned[i]) continue;
                    var tk = _obsSharedTickets[i];
                    if (tk.ticket == 0UL) continue;
                    if (tk.slotIndex >= ObsSharedSlotCountV1)
                    { MarkObsSharedStopTerminal("committed_packet_malformed_slot"); return false; }
                    if (stop.boundEpoch != tk.epoch || stop.boundGeneration != tk.generation) continue;
                    uint slotState = tk.slotIndex == 0u ? stats.slotState0
                        : (tk.slotIndex == 1u ? stats.slotState1 : stats.slotState2);
                    if (slotState != ObsSharedBridgeSlotStateCommittedV1) continue;   // exact COMMITTED=3 ONLY; FREE→pass 2, READING/INFLIGHT/unknown→never discarded here
                    Exception cf = TryDiscardObsSharedCommittedUnread(i);
                    if (cf != null) _obsSharedStopError = Combine(_obsSharedStopError, cf);
                }
            }

            if (!ObsSharedLedgerProvenEmpty()) { reason = "ledger_not_empty_pending_worker_drain"; return false; }
            if (stop.boundEpoch != _obsSharedStopBoundEpoch ||
                stop.boundGeneration != (uint)_obsSharedStopBoundGeneration)
            { reason = "stop_bound_identity_not_original"; return false; }
            if ((stop.flags & ObsSharedStopFlagsWorkerDrainedV1) == 0u ||
                (stop.flags & ObsSharedStopFlagsBridgeClosedV1) == 0u ||
                stop.pendingSlots != 0u)
            { reason = "bridge_not_closed_and_worker_drained"; return false; }
            if (!ObsSharedAllBridgeSlotsFree(stats)) { reason = "bridge_slots_not_all_free"; return false; }
            if (ho.CaptureBusy || _tickInProgress || _obsSharedFinallyActive)
            { reason = "busy_before_finalize"; return false; }
            // kernel-render-120: SOURCE_UI finalize may only start while the scheduler is
            // quiet-proven — ledger DISCARD passes above stay ungated so the native 1000 ms drain
            // deadline is never starved by a pending kernel stop.
            bool schedTerminal;
            string schedReason;
            if (!ObsSharedKernelQuietVerified(out schedReason, out schedTerminal))
            {
                if (schedTerminal)
                { MarkObsSharedStopTerminal("scheduler_quiet_terminal " + schedReason); return false; }
                reason = "scheduler_quiet_pending " + schedReason;
                return false;
            }
            return TryFinalizeObsSharedSourceUIStep(out reason);
        }

        /// <summary>Host confirmation of the ACTUAL native teardown for the frozen original shared
        /// stop. Requires source-ready + destroyRc==0 + nativeState OFF(0) + original identity, no
        /// owner capture busy and the whole tick/finally exited, and THEN the original owner's pure
        /// managed TryCompleteStop(originalBoundGeneration) returning true. Only that combination
        /// marks the whole stop verified and the final _stopStatus "stopped". Exception/pending
        /// clears nothing (packets/owner/refs retained) and never reopens anything.</summary>
        internal bool ConfirmObsSharedNativeDestroyed(int destroyRc, int nativeState)
        {
            if (!_obsSharedRequested || !_obsSharedStopEntered) return false;
            if (_obsSharedWholeStopVerified) return true;
            if (_obsSharedStopTerminal) return false;
            if (!_obsSharedSourceReady) return false;
            if (_obsSharedStopBoundGeneration <= 0) return false;   // gen0 can never become wholeStopVerified
            if (destroyRc != 0 || nativeState != 0) return false;   // ACTUAL rc0 + native OFF only
            var ho = _obsSharedStopOwner;
            if (ho == null || !ReferenceEquals(_hostedOwner, ho) ||
                _obsSharedStopBoundGeneration != ho.BoundGeneration) return false;
            if (ho.CaptureBusy || _tickInProgress || _obsSharedFinallyActive) return false;
            if (!_obsSharedNeverBoundSourceReady && !ObsSharedLedgerProvenEmpty()) return false;
            // kernel-render-120: whole-stop confirmation ALSO requires the quiet proof — TryStep
            // gates upstream, this is the honest final gate before TryCompleteStop.
            bool confirmSchedTerminal;
            string confirmSchedReason;
            if (!ObsSharedKernelQuietVerified(out confirmSchedReason, out confirmSchedTerminal))
            {
                if (confirmSchedTerminal)
                    MarkObsSharedStopTerminal("scheduler_quiet_terminal " + confirmSchedReason);
                return false;
            }
            bool closed;
            try { closed = ho.TryCompleteStop(_obsSharedStopBoundGeneration); }
            catch (Exception ex)
            {
                _obsSharedStopError = Combine(_obsSharedStopError, ex);
                return false;   // exception retains every ref/packet — no partial completion claimed
            }
            if (!closed) return false;   // still pending — the host retries the confirmation
            _obsSharedWholeStopVerified = true;
            _hostedStopPending = false;
            _stopStatus = "stopped";
            _host.Log("VC_PRODUCER obs_shared_whole_stop_verified generation=" +
                _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture) +
                " destroyRc=0 nativeState=0 tryCompleteStop=true ownerRetainedByDesign" +
                " finalStopSnapshotValid=" + (_obsSharedFinalStopStateValid ? "true" : "false") +
                " neverBound=" + (_obsSharedNeverBoundSourceReady ? "true" : "false"));
            return true;
        }

        /// <summary>Contract-6 never-bound proof: ONLY with actual Stop32 boundEpoch0/gen0/pending0/
        /// flags0, actual bridgeStats initCount0 with all three slots FREE, a proven-empty ledger
        /// carrying no ticket ever, the original owner capture-busy false, the whole tick exited AND
        /// zero capture evidence for the generation (no PREPARE granted/COMMIT accepted/rejected ever
        /// recorded). Any nonzero metadata forbids this bypass forever. Sets the DISTINCT never-bound
        /// flag — this is NOT a SourceUIFinalize rc==0 and stores no final snapshot.</summary>
        private bool TryProveObsSharedNeverBoundStep(NC04.NC01HostedSceneOwner owner, out string reason)
        {
            reason = null;
            if (owner.CaptureBusy || _tickInProgress || _obsSharedFinallyActive)
            { reason = "never_bound_proof_busy_or_tick_on_stack"; return false; }
            if (_obsSharedPrepared != 0UL || _obsSharedCommitAccepted != 0UL || _obsSharedRejected != 0UL)
            {
                MarkObsSharedStopTerminal("never_bound_path_forbidden_after_capture_evidence");
                return false;
            }
            _obsSharedNeverBoundSourceReady = true;
            _obsSharedSourceReady = true;
            _host.Log("VC_PRODUCER obs_shared_stop_never_bound_source_ready boundGeneration=" +
                _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture) +
                " stop32=allZero statsInit0=slotsFree ledgerEmpty noTicketEver ownerBusy=false —" +
                " distinct never_bound proof, NOT SourceUIFinalize rc0, no native source lease," +
                " whole_stop_NOT_yet (host: destroy is a no-op confirm path)");
            return true;
        }

        /// <summary>Real same-source-UI finalize step: one FinalizeSharedGpuSourceUI call, then a
        /// REAL fresh ReadSharedGpuStop verification (flags==7 exactly, pending==0, ORIGINAL
        /// epoch+generation). Only a matching readback stores the actual snapshot and marks the
        /// source ready; rc!=0/Pending and any mismatch are honest outcomes — finalize rc==0 is
        /// NEVER inferred and a mismatch is a false-close refusal (terminal).</summary>
        private bool TryFinalizeObsSharedSourceUIStep(out string reason)
        {
            reason = null;
            int frc;
            try { frc = _host.FinalizeSharedGpuSourceUI(); }
            catch (Exception ex)
            {
                _obsSharedStopError = Combine(_obsSharedStopError, ex);
                MarkObsSharedStopTerminal("source_ui_finalize_exception " + ex.GetType().Name);
                return false;
            }
            if (frc != (int)VRSIMPlugin.SharedGpuResultV1.Completed)
            {
                _obsSharedStopError = Combine(_obsSharedStopError, new InvalidOperationException(
                    "OBS_SHARED_SOURCE_UI_FINALIZE_NOT_COMPLETED rc=" + frc.ToString(CultureInfo.InvariantCulture)));
                if (frc == (int)VRSIMPlugin.SharedGpuResultV1.Pending) { reason = "source_ui_finalize_pending"; return false; }
                // FinalizeUnavailable(-2) may NEVER be treated as a finalize rc==0: this path is
                // reached only while real bound metadata exists, so the refusal is a quarantine.
                MarkObsSharedStopTerminal("source_ui_finalize_refused rc=" +
                    frc.ToString(CultureInfo.InvariantCulture) + " (-2=FinalizeUnavailable never equals rc0)");
                return false;
            }
            VRSIMPlugin.SharedGpuStopStateV1 verify;
            int vrc;
            try { vrc = _host.ReadSharedGpuStop(out verify); }
            catch (Exception ex)
            {
                _obsSharedStopError = Combine(_obsSharedStopError, ex);
                MarkObsSharedStopTerminal("post_finalize_stop_read_exception " + ex.GetType().Name);
                return false;
            }
            bool verified =
                vrc == (int)VRSIMPlugin.SharedGpuResultV1.Completed &&
                verify.flags == ObsSharedStopFlagsSourceFinalizedV1 &&
                verify.pendingSlots == 0u &&
                verify.boundEpoch == _obsSharedStopBoundEpoch &&
                verify.boundGeneration == (uint)_obsSharedStopBoundGeneration;
            if (!verified)
            {
                _obsSharedStopError = Combine(_obsSharedStopError, new InvalidOperationException(
                    "OBS_SHARED_FINALIZE_READBACK_MISMATCH rc=" + vrc.ToString(CultureInfo.InvariantCulture) +
                    " flags=" + verify.flags.ToString(CultureInfo.InvariantCulture) +
                    " pending=" + verify.pendingSlots.ToString(CultureInfo.InvariantCulture) +
                    " boundEpoch=" + verify.boundEpoch.ToString(CultureInfo.InvariantCulture) +
                    " boundGen=" + verify.boundGeneration.ToString(CultureInfo.InvariantCulture) +
                    " expectedEpoch=" + _obsSharedStopBoundEpoch.ToString(CultureInfo.InvariantCulture) +
                    " expectedGen=" + _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture)));
                MarkObsSharedStopTerminal("finalize_rc0_readback_mismatch_false_close_refused");
                return false;
            }
            _obsSharedFinalStopState = verify;
            _obsSharedFinalStopStateValid = true;
            _obsSharedSourceReady = true;
            _host.Log("VC_PRODUCER obs_shared_source_ui_finalized_verified generation=" +
                _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture) +
                " finalizeRc=0 readback flags=7 pending=0 origIdentity=true snapshotStored=true —" +
                " source ready for native teardown; whole_stop_NOT_yet (no UI nativeDestroy here;" +
                " native worker owns context exit; host confirms destroy via ConfirmObsSharedNativeDestroyed)");
            return true;
        }

        // Shared-stop extension of the 3A exact-DISCARD pattern to a COMMITTED-but-unread packet
        // (owned=false, committed=true) that pass 3 reaches ONLY when the bridge is CLOSED and the
        // cached slot state is EXACTLY COMMITTED(3) — READING/INFLIGHT/unknown states are never
        // discarded from the UI. Uses ONLY
        // the retained complete ticket + original identity — never rewritten to the current
        // _generation/epoch — and never reruns the owner. Returns null ONLY for an actual native
        // rc==0 (entry cleared); Pending/error returns the REAL fault (original exception object or
        // a synthetic rc fault naming the ticket + ORIGINAL identity generation) with ticket,
        // identity and the committed flag RETAINED. Never Records, never swallows.
        private Exception TryDiscardObsSharedCommittedUnread(int ledgerIndex)
        {
            if (ledgerIndex < 0 || ledgerIndex >= _obsSharedCommitted.Length) return null;
            if (!_obsSharedCommitted[ledgerIndex] || _obsSharedOwned[ledgerIndex]) return null;
            var ticket = _obsSharedTickets[ledgerIndex];
            if (ticket.ticket == 0UL) { _obsSharedCommitted[ledgerIndex] = false; return null; }   // nothing to discard
            int origGen = (int)_obsSharedIdentities[ledgerIndex].generation;
            Exception discardError = null;
            int drc = (int)VRSIMPlugin.SharedGpuResultV1.Error;
            try { drc = _host.DiscardSharedGpu(ref ticket); }
            catch (Exception dex) { discardError = dex; }
            if (discardError == null && drc == (int)VRSIMPlugin.SharedGpuResultV1.Completed)
            {
                // actual native rc==0 is the ONLY path that clears a committed entry.
                _obsSharedCommitted[ledgerIndex] = false;
                _obsSharedTickets[ledgerIndex] = default(VRSIMPlugin.SharedGpuTicketV1);
                _obsSharedIdentities[ledgerIndex] = default(VRSIMPlugin.SharedGpuCaptureIdentityV1);
                return null;
            }
            if (discardError != null) return discardError;
            return new InvalidOperationException(
                "OBS_SHARED_COMMITTED_DISCARD_NOT_COMPLETED rc=" + drc.ToString(CultureInfo.InvariantCulture) +
                " ticket=" + ticket.ticket.ToString(CultureInfo.InvariantCulture) +
                " identityGeneration=" + origGen.ToString(CultureInfo.InvariantCulture));
        }

        // Proven-empty ledger: no owned flag, no committed flag and no nonzero ticket anywhere. This
        // NEVER clears storage to fake emptiness — entries clear only through the real rc0/FREE paths.
        private bool ObsSharedLedgerProvenEmpty()
        {
            for (int i = 0; i < _obsSharedTickets.Length; i++)
                if (_obsSharedOwned[i] || _obsSharedCommitted[i] || _obsSharedTickets[i].ticket != 0UL)
                    return false;
            return true;
        }

        private bool ObsSharedAllBridgeSlotsFree(VRSIMPlugin.SharedGpuBridgeStatsV1 stats)
        {
            return stats.slotState0 == ObsSharedBridgeSlotStateFreeV1 &&
                stats.slotState1 == ObsSharedBridgeSlotStateFreeV1 &&
                stats.slotState2 == ObsSharedBridgeSlotStateFreeV1;
        }

        private void MarkObsSharedStopTerminal(string why)
        {
            if (_obsSharedStopTerminal) return;
            _obsSharedStopTerminal = true;
            _host.Log("VC_PRODUCER obs_shared_stop_terminal reason=" + why +
                " boundGeneration=" + _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture) +
                " steps=" + _obsSharedSourceStopSteps.ToString(CultureInfo.InvariantCulture) +
                " — refs/packet/error RETAINED; host must stop automatic retry; Enter stays blocked;" +
                " never reopened, never cleared to fake a receipt");
        }

        /// <summary>OBS-SHARED StopHostedCore branch (Card3B1). The first-entry admission close (pure
        /// managed RequestStop on the frozen original generation + EXACTLY-ONE metadata-only native
        /// drain request) is idempotent here — it normally already ran inside the same
        /// StopAndRestore/capture-failure call via CloseObsSharedFirstStopAdmissionOnce. Every Stop
        /// turn then runs at most ONE finite source step
        /// and ALWAYS leaves an honest pending/terminal throw that retains producer + owner + packets
        /// until ConfirmObsSharedNativeDestroyed completed the whole stop. NEVER returns through the
        /// old ho.StopVerified / generic owner-only completion and NEVER forces the busy flag.</summary>
        private void StopHostedCoreObsShared(NC04.NC01HostedSceneOwner ho)
        {
            if (_obsSharedWholeStopVerified)
            {
                _hostedStopPending = false;
                _stopStatus = "stopped";
                return;
            }
            // Idempotent once-per-original-stop close: a no-op when the same StopAndRestore or
            // capture-failure call already froze and closed admission; still the fallback here if the
            // first close could not run before this point.
            CloseObsSharedFirstStopAdmissionOnce();
            string stepReason;
            Exception stepException = null;
            try
            {
                TryStepObsSharedSourceStop(out stepReason);
            }
            catch (Exception sex)
            {
                stepException = sex;
                stepReason = "step_exception " + sex.GetType().Name + ": " + sex.Message;
            }
            if (stepException != null) _obsSharedStopError = Combine(_obsSharedStopError, stepException);
            // Whole-unverified — shared stop stays PENDING/THROW by design: the source must never be
            // dropped here and the generic verified path above is never reached.
            _hostedStopPending = true;
            _stopStatus = _obsSharedStopTerminal ? "obs_shared_stop_terminal_retained" : "obs_shared_stop_pending";
            throw new InvalidOperationException(
                "VC_PRODUCER hosted OBS-SHARED stop " +
                (_obsSharedStopTerminal
                    ? "TERMINAL (retries must stop; ObsSharedStopTerminal=true; refs retained)"
                    : "pending (retry via the host 100 ms deferred source steps)") +
                " generation=" + _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture) +
                " stepReason=" + (stepReason ?? "none") +
                " sourceReady=" + (_obsSharedSourceReady ? "true" : "false") +
                " wholeVerified=false steps=" + _obsSharedSourceStopSteps.ToString(CultureInfo.InvariantCulture) +
                " drainRc=" + _obsSharedDrainRequestRc.ToString(CultureInfo.InvariantCulture) +
                ". Producer/owner/packet refs RETAINED; no native destroy and no generic owner-only completion here.");
        }

        /// <summary>Pure managed completion attempt from the Tick finally (NO SDK getter, NO
        /// capture/cache/submit): once this tick's entered SDK transaction has exited, a pending
        /// hosted stop may verify here. Still busy → stays pending for the next Stop retry.
        /// NC02-XR-P3 staged: completion additionally requires the helper drained and zero
        /// staging-pending proven via PURE managed reads only — no drain step from here.</summary>
        private void TryCompleteHostedStopAfterTick()
        {
            var ho = _hostedOwner;
            int stopGen = _hostedStopBoundGeneration;
            if (ho == null || stopGen <= 0) return;
            // OBS-SHARED (Card3B1): the shared stop leaves ONLY a pending state for the next
            // dispatcher turn here — no drain step, no native finalize and never the generic
            // owner-only StopVerified/TryCompleteStop completion below. The busy flag is never
            // forced false; the host deferred retry drives TryStepObsSharedSourceStop.
            if (_obsSharedRequested && _obsSharedStopEntered) return;
            if (_nc02StagingRequested &&
                (_tickInProgress || !ho.StagingHelperDrained || ho.StagingPendingCount != 0))
                return;   // staged helper not provably closed — pending; the next host 100 ms Stop retry advances the drain
            bool verified;
            try { verified = ho.StopVerified || ho.TryCompleteStop(stopGen); }
            catch (Exception ex)
            {
                _host.Log("VC_PRODUCER hosted_stop_complete_fail " + ex.GetType().Name + ": " + ex.Message);
                return;
            }
            if (!verified) return;   // still busy — the Host/harness StopAndRestore retry completes it
            if (_nc02StagingRequested)
            {
                _nc02CaptureRows.Clear();
                Array.Clear(_nc02MapPairIds, 0, _nc02MapPairIds.Length);
                Array.Clear(_nc02MapFrameSerials, 0, _nc02MapFrameSerials.Length);
                Array.Clear(_nc02PollCounts, 0, _nc02PollCounts.Length);
            }
            _hostedStopPending = false;
            _stopStatus = "stopped";
            _host.Log("VC_PRODUCER hosted_stop_completed_after_tick generation=" + stopGen.ToString(CultureInfo.InvariantCulture) +
                " ownerRetainedByDesign=true" +
                (_nc02StagingRequested ? " nc02_staging_rows_maps_cleared=true" : ""));
        }

        private System.Collections.Generic.IEnumerable<StereoResampleWorker> WorkersToStop(StereoResampleWorker current)
        {
            if (current != null) yield return current;
            if (_unjoinedWorker != null && !ReferenceEquals(_unjoinedWorker, current)) yield return _unjoinedWorker;
        }

        private static Exception Combine(Exception first, Exception second)
        {
            if (first == null) return second;
            if (second == null) return first;
            return new AggregateException(first, second);
        }

        // ---------------- capture metadata: frameSerial -> immutable row (bounded 8, oldest evicted) ----------------
        private sealed class CaptureMetaRow
        {
            internal readonly ulong FrameSerial;
            internal readonly ulong PoseSerial;
            internal readonly int Generation;
            internal readonly double[] CameraLeftPosition;   // cloned P_Lcam (SIM mm) at capture time
            internal readonly double CapL;                   // left owner-capture duration ms (NaN in paired mode)
            internal readonly double CapR;                   // right owner-capture duration ms (NaN in paired mode)
            internal readonly double CapturePairMs;          // paired capture+alpha ms; NaN = not a paired capture
            internal readonly long CaptureStartQpc;          // QPC stamp taken BEFORE the first capture
            internal CaptureMetaRow(ulong frameSerial, ulong poseSerial, int generation,
                double[] cameraLeftPosition, double capL, double capR, long captureStartQpc,
                double capturePairMs = double.NaN)
            {
                FrameSerial = frameSerial;
                PoseSerial = poseSerial;
                Generation = generation;
                CameraLeftPosition = (double[])cameraLeftPosition.Clone();
                CapL = capL;
                CapR = capR;
                CapturePairMs = capturePairMs;
                CaptureStartQpc = captureStartQpc;
            }
        }

        private void StoreCaptureMeta(CaptureMetaRow row)
        {
            if (!_captureMeta.ContainsKey(row.FrameSerial)) _captureMetaOrder.Add(row.FrameSerial);
            _captureMeta[row.FrameSerial] = row;
            while (_captureMetaOrder.Count > MaxCaptureMetaRows)
            {
                ulong oldest = _captureMetaOrder[0];
                _captureMetaOrder.RemoveAt(0);
                _captureMeta.Remove(oldest);
                _metaEvicted++;
            }
        }

        private void RemoveMeta(ulong frameSerial)
        {
            if (_captureMeta.Remove(frameSerial)) _captureMetaOrder.Remove(frameSerial);
        }

        // ---------------- poll ≤1 completed resample result (UI thread, before fresh poses) ----------------
        private void PollCompletedFrame()
        {
            if (!_running) return;
            var worker = _worker;
            if (worker == null) return;
            StereoResampleResult completed;
            if (!worker.TryTakeCompleted(out completed)) return;

            CaptureMetaRow meta;
            bool haveMeta = _captureMeta.TryGetValue(completed.FrameSerial, out meta);

            // ---- guards: every rejection retires the metadata where available and counts honestly ----
            if (completed.Generation != _generation)
            {
                _staleDropped++;
                if (haveMeta) RemoveMeta(completed.FrameSerial);
                return;
            }
            if (!haveMeta) { _metaMissing++; return; }
            if (meta.Generation != _generation)
            {
                _staleDropped++;
                RemoveMeta(completed.FrameSerial);
                return;
            }
            if (completed.FrameSerial <= _lastCompletedFrameSerial)
            {
                _staleDropped++;
                RemoveMeta(completed.FrameSerial);
                return;
            }
            if (completed.PoseSerial != meta.PoseSerial)
            {
                _metaMismatch++;
                RemoveMeta(completed.FrameSerial);
                _host.Log("VC_ASYNC_REJECT reason=pose_serial_mismatch frameSerial=" + completed.FrameSerial +
                    " resultPose=" + completed.PoseSerial + " metaPose=" + meta.PoseSerial);
                return;
            }
            if (completed.FrameSerial == 0UL || completed.PoseSerial == 0UL)
            {
                _metaMismatch++;
                RemoveMeta(completed.FrameSerial);
                _host.Log("VC_ASYNC_REJECT reason=zero_serial frameSerial=" + completed.FrameSerial +
                    " poseSerial=" + completed.PoseSerial);
                return;
            }
            if (completed.LeftBgra == null || completed.RightBgra == null ||
                completed.LeftBgra.Length != CaptureProfile.PayloadBytes ||
                completed.RightBgra.Length != CaptureProfile.PayloadBytes)
            {
                _workerFailures++;
                RemoveMeta(completed.FrameSerial);
                _host.Log("VC_ASYNC_REJECT reason=payload frameSerial=" + completed.FrameSerial +
                    " leftNull=" + (completed.LeftBgra == null ? "true" : "false") +
                    " rightNull=" + (completed.RightBgra == null ? "true" : "false") +
                    " errorNull=" + (completed.Error == null ? "true" : "false"));
                return;
            }
            if (completed.Error != null)
            {
                _workerFailures++;
                RemoveMeta(completed.FrameSerial);
                _host.Log("VC_ASYNC_WORKER_FAIL frameSerial=" + completed.FrameSerial +
                    " " + completed.Error.GetType().Name + ": " + completed.Error.Message);
                return;
            }

            // ---- all guards passed: submit EXACTLY the polled pair with the RESULT's serials.
            // Never re-bound to a fresher pose; _frameSerial stays the wire allocator untouched here. ----
            _lastCompletedFrameSerial = completed.FrameSerial;
            bool submitted = false;
            long submitEndQpc = 0;
            try
            {
                try
                {
                    long submitStartQpc = Stopwatch.GetTimestamp();
                    int rc = _host.SubmitStereo(completed.LeftBgra, completed.RightBgra,
                        FrameW, FrameH, FrameW * 4, completed.FrameSerial, completed.PoseSerial);
                    submitEndQpc = Stopwatch.GetTimestamp();
                    if (rc == 0) submitted = true;
                    else
                    {
                        _submitFailures++;
                        _host.Log("VC_ASYNC_SUBMIT_FAIL frameSerial=" + completed.FrameSerial +
                            " poseSerial=" + completed.PoseSerial + " rc=" + rc);
                    }
                }
                catch (Exception ex)
                {
                    _submitFailures++;
                    _host.Log("VC_ASYNC_SUBMIT_EXCEPTION frameSerial=" + completed.FrameSerial +
                        " " + ex.GetType().Name + ": " + ex.Message);
                }
            }
            finally
            {
                // The polled result retires its metadata row even when the submit throws.
                RemoveMeta(completed.FrameSerial);
            }
            if (!submitted) return;   // no success counter, no evidence, no performance sample

            _framesSubmitted++;
            _liveSubmitted++;
            _pairMs = (submitEndQpc - meta.CaptureStartQpc) * 1000.0 / Stopwatch.Frequency;
            NoteAsyncSuccess(meta, completed, submitEndQpc);

            // Evidence IO AFTER the successful submit; IO failure never undoes the counted submit.
            try
            {
                if (!_evidencePairSaved)
                {
                    _evidencePairSaved = true;
                    SavePng("first-stereo-left.png", completed.LeftBgra);
                    SavePng("first-stereo-right.png", completed.RightBgra);
                    WriteJson("first-stereo-evidence.json", "{\"evidence\":\"first_live_stereo_pair\"" +
                        ",\"pipeline\":\"async_worker_resample_completion_submit\"" +
                        ",\"frameSerial\":" + completed.FrameSerial +
                        ",\"poseSerial\":" + completed.PoseSerial +
                        ",\"generation\":" + _generation +
                        ",\"captureToSubmitMs\":" + _pairMs.ToString("0.0", CultureInfo.InvariantCulture) +
                        ",\"size\":\"" + FrameW + "x" + FrameH + "\"" +
                        ",\"payloadBytes\":" + CaptureProfile.PayloadBytes +
                        ",\"leftPng\":\"first-stereo-left.png\",\"rightPng\":\"first-stereo-right.png\"" +
                        ",\"binding\":\"submitted pair and saved PNGs are the SAME completed worker result; serials are the RESULT's, never re-bound to a fresher pose\"" +
                        ",\"capturedUtc\":\"" + DateTime.UtcNow.ToString("o") + "\"}");
                    _host.Log("FIRST_STEREO_EVIDENCE saved frameSerial=" + completed.FrameSerial +
                        " poseSerial=" + completed.PoseSerial);
                }
                SaveMotionEvidence(completed.LeftBgra, meta.CameraLeftPosition);
            }
            catch (Exception ex)
            {
                _host.Log("VC_ASYNC_EVIDENCE_FAIL " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        private void ResetAsyncTelemetry()
        {
            _asyncWindowStartQpc = 0;
            _asyncLastSubmitQpc = 0;
            _asyncContentCount = 0;
            _asyncSeedFrameExcluded = false;   // until the first successful live submit seeds the window
            _asyncBaseValid = false;   // delta anchor re-seeds at the window's first successful LIVE submit
            _asyncFinalWritten = false;
            _asyncIntervals.Clear();
            _asyncCapLs.Clear();
            _asyncCapRs.Clear();
            _asyncCapPairs.Clear();
            _asyncRsLs.Clear();
            _asyncRsRs.Clear();
            _asyncCapToSubmits.Clear();
        }

        /// <summary>Telemetry note for ONE successful LIVE submit (rc==0 only — the static
        /// pre-calibration preview never reaches here). Updates the per-window async-pipeline
        /// lists and counts, each capped at 300 items. The interval series uses ONLY live
        /// successful submit QPC stamps; the window starts at the FIRST actual successful live
        /// submit and the native-stats delta anchor is seeded at that same instant. No file is
        /// written here — see WriteAsyncWindow (called from Tick / StopAndRestore).</summary>
        private void NoteAsyncSuccess(CaptureMetaRow meta, StereoResampleResult result, long submittedQpc)
        {
            if (_asyncWindowStartQpc == 0)
            {
                _asyncWindowStartQpc = submittedQpc;
                // Seed-frame rule owned HERE (not by the anchor): true only for the initial seed
                // frame — a window starting with no prior submit stamp excludes this first submit.
                _asyncSeedFrameExcluded = _asyncLastSubmitQpc == 0;
                AnchorAsyncNativeBaseline();
            }
            if (_asyncLastSubmitQpc != 0)
                _asyncIntervals.Add((submittedQpc - _asyncLastSubmitQpc) * 1000.0 / Stopwatch.Frequency);
            _asyncLastSubmitQpc = submittedQpc;
            if (!double.IsNaN(meta.CapL) && !double.IsInfinity(meta.CapL) && meta.CapL >= 0.0)
                _asyncCapLs.Add(meta.CapL);
            if (!double.IsNaN(meta.CapR) && !double.IsInfinity(meta.CapR) && meta.CapR >= 0.0)
                _asyncCapRs.Add(meta.CapR);
            if (!double.IsNaN(meta.CapturePairMs) && !double.IsInfinity(meta.CapturePairMs) && meta.CapturePairMs >= 0.0)
                _asyncCapPairs.Add(meta.CapturePairMs);
            _asyncRsLs.Add(result.LeftResampleMs);
            _asyncRsRs.Add(result.RightResampleMs);
            _asyncCapToSubmits.Add((submittedQpc - meta.CaptureStartQpc) * 1000.0 / Stopwatch.Frequency);
            TrimAsyncWindowList(_asyncIntervals);
            TrimAsyncWindowList(_asyncCapLs);
            TrimAsyncWindowList(_asyncCapRs);
            TrimAsyncWindowList(_asyncCapPairs);
            TrimAsyncWindowList(_asyncRsLs);
            TrimAsyncWindowList(_asyncRsRs);
            TrimAsyncWindowList(_asyncCapToSubmits);
            _asyncContentCount++;
        }

        private static void TrimAsyncWindowList<T>(System.Collections.Generic.List<T> list)
        {
            if (list.Count > 300) list.RemoveRange(0, list.Count - 300);
        }

        /// <summary>Seed the native-stats delta anchor for the current window from the SDK
        /// counters (ulong d3dUploads / poseImageMismatch, overflow-guarded — never stored as
        /// negative or overflowing values). Does NOT touch _asyncSeedFrameExcluded: a
        /// native-unavailable or regression re-anchor must never change the event-count window
        /// definition.</summary>
        private void AnchorAsyncNativeBaseline()
        {
            _asyncBaseValid = false;
            var st = _host.ReadStereoStats();
            if (st.HasValue && st.Value.d3dUploads <= (ulong)long.MaxValue &&
                st.Value.poseImageMismatch <= (ulong)long.MaxValue)
            {
                _asyncBaseD3dUploads = (long)st.Value.d3dUploads;
                _asyncBasePoseMismatch = (long)st.Value.poseImageMismatch;
                _asyncBaseValid = true;
            }
        }

        /// <summary>Write the async-pipeline telemetry window (periodic ≥10 s, or the single
        /// final flush) to async-pipeline-10s.json. SUCCESSFUL LIVE SUBMITS ONLY — no fake
        /// samples; native counters are reported ONLY as guarded monotonic deltas (regression →
        /// unknown + re-anchor, never a zeroed PASS). After a periodic write the window restarts
        /// at THIS boundary (submit-interval stamps kept) and the native anchor re-seeds here.</summary>
        private void WriteAsyncWindow(long nowQpc, bool final)
        {
            if (_asyncWindowStartQpc == 0 || _asyncFinalWritten) return;
            long elapsedQpc = nowQpc - _asyncWindowStartQpc;
            if (!final && elapsedQpc < AsyncWindowQpc) return;
            double elapsedMs = elapsedQpc * 1000.0 / Stopwatch.Frequency;
            double elapsedSec = elapsedMs / 1000.0;
            // FIRST anchored window: the seed submit (window start) is not window throughput.
            long windowSuccesses = _asyncSeedFrameExcluded
                ? Math.Max(0L, _asyncContentCount - 1L) : (long)_asyncContentCount;
            double submitFps = windowSuccesses / elapsedSec;

            long? uploadDelta = null, mismatchDelta = null;
            string nativeStatus;
            var st = _host.ReadStereoStats();
            bool curOk = st.HasValue && st.Value.d3dUploads <= (ulong)long.MaxValue &&
                st.Value.poseImageMismatch <= (ulong)long.MaxValue;
            if (!curOk) nativeStatus = "native_stats_unavailable";
            else if (!_asyncBaseValid)
            {
                nativeStatus = "native_baseline_unavailable_reanchor";
                AnchorAsyncNativeBaseline();
            }
            else
            {
                long curU = (long)st.Value.d3dUploads, curM = (long)st.Value.poseImageMismatch;
                if (curU < _asyncBaseD3dUploads || curM < _asyncBasePoseMismatch)
                {
                    nativeStatus = "native_counter_regression_reanchor";
                    AnchorAsyncNativeBaseline();   // re-anchor; this window stays unknown
                }
                else
                {
                    uploadDelta = curU - _asyncBaseD3dUploads;
                    mismatchDelta = curM - _asyncBasePoseMismatch;
                    nativeStatus = "ok";
                }
            }
            double? uploadFps = uploadDelta.HasValue && elapsedSec > 0.0
                ? (double?)((double)uploadDelta.Value / elapsedSec) : null;

            var sb = new StringBuilder();
            sb.Append("{\"schema\":\"async-pipeline-10s/1\"")
              .Append(",\"scope\":\"actual_success_live_submit_not_verified_display\"")
              .Append(",\"kind\":").Append(final ? "\"final\"" : "\"periodic_10s\"")
              .Append(",\"clock\":\"Stopwatch QPC (monotonic)\"")
              .Append(",\"generation\":").Append(_generation)
              .Append(",\"selected\":{\"profile\":\"").Append(CaptureProfile.Selected)
              .Append("\",\"width\":").Append(FrameW)
              .Append(",\"height\":").Append(FrameH)
              .Append(",\"payloadBytes\":").Append(CaptureProfile.PayloadBytes)
              .Append(",\"nominalTickMs\":").Append(JsonNumber(_nominalTickMs)).Append('}')
              .Append(",\"elapsedMs\":").Append(JsonNumber(elapsedMs))
              .Append(",\"windowSuccessfulLiveSubmissions\":").Append(_asyncContentCount)
              .Append(",\"windowSeedFrameExcluded\":").Append(_asyncSeedFrameExcluded ? "true" : "false")
              .Append(",\"cumulativeLiveSubmitted\":").Append(_liveSubmitted)
              .Append(",\"cumulativeAllSuccess\":").Append(_framesSubmitted)
              .Append(",\"elapsedSuccessfulSubmitFps\":").Append(JsonNumber(submitFps))
              .Append(",\"uniqueNativeUploadDelta\":").Append(JsonLong(uploadDelta))
              .Append(",\"uniqueNativeUploadFps\":").Append(JsonNumber(uploadFps))
              .Append(",\"poseImageMismatchDelta\":").Append(JsonLong(mismatchDelta))
              .Append(",\"nativeStatus\":\"").Append(nativeStatus).Append('"');
            var w = _worker ?? _unjoinedWorker ?? _lastWorker;
            if (w == null) sb.Append(",\"worker\":null");
            else sb.Append(",\"worker\":{\"pending\":").Append(w.PendingCount)
                  .Append(",\"inFlight\":").Append(w.InFlightCount)
                  .Append(",\"completed\":").Append(w.CompletedCount)
                  .Append(",\"droppedPending\":").Append(w.DroppedPending)
                  .Append(",\"droppedCompleted\":").Append(w.DroppedCompleted)
                  .Append(",\"rejectedJobs\":").Append(w.RejectedJobs)
                  .Append(",\"stopped\":").Append(w.IsStopped ? "true" : "false")
                  .Append(",\"hasExited\":").Append(w.HasExited ? "true" : "false").Append('}');
            sb.Append(",\"metadata\":{\"missing\":").Append(_metaMissing)
              .Append(",\"mismatch\":").Append(_metaMismatch)
              .Append(",\"evicted\":").Append(_metaEvicted)
              .Append(",\"staleDropped\":").Append(_staleDropped)
              .Append(",\"workerFailures\":").Append(_workerFailures)
              .Append(",\"submitFailures\":").Append(_submitFailures)
              .Append(",\"jobRejectLogCount\":").Append(_jobRejectLogCount).Append('}')
              .Append(",\"stopStatus\":\"").Append(_stopStatus).Append('"')
              .Append(",\"windowListCap\":300");
            AppendPercentiles(sb, "submitIntervalMs", _asyncIntervals);
            AppendPercentiles(sb, "captureLeftMs", _asyncCapLs);
            AppendPercentiles(sb, "captureRightMs", _asyncCapRs);
            AppendPercentiles(sb, "capturePairMs", _asyncCapPairs);
            AppendPercentiles(sb, "resampleLeftMs", _asyncRsLs);
            AppendPercentiles(sb, "resampleRightMs", _asyncRsRs);
            AppendPercentiles(sb, "captureToSubmitMs", _asyncCapToSubmits);
            sb.Append(",\"capturedUtc\":\"").Append(DateTime.UtcNow.ToString("o")).Append("\"}");
            WriteJson("async-pipeline-10s.json", sb.ToString());

            if (final) { _asyncFinalWritten = true; return; }
            _asyncIntervals.Clear(); _asyncCapLs.Clear(); _asyncCapRs.Clear();
            _asyncCapPairs.Clear();
            _asyncRsLs.Clear(); _asyncRsRs.Clear(); _asyncCapToSubmits.Clear();
            _asyncContentCount = 0;
            _asyncWindowStartQpc = nowQpc;   // next window starts at this write boundary;
                                             // _asyncLastSubmitQpc is KEPT for interval continuity
            _asyncSeedFrameExcluded = false; // boundary-anchored window (count 0): no seed subtraction
            AnchorAsyncNativeBaseline();     // re-anchor native stats at the SAME boundary
        }

        // ---------------- H2 raw-GPU telemetry -> gpu-pipeline-10s.json (GPU opt-in ONLY) ----------------
        private void ResetGpuTelemetry()
        {
            _gpuWindowStartQpc = 0;
            _gpuLastEnqueueQpc = 0;
            _gpuEnqueueCount = 0;
            _gpuSeedFrameExcluded = false;
            _gpuBaseValid = false;
            _gpuFinalWritten = false;
            _gpuBaseCommitted = 0; _gpuBaseLayerOk = 0; _gpuBasePairFailed = 0; _gpuBasePoseRejected = 0;
            _gpuBaseEpoch = 0UL;
            _gpuCapPairs.Clear();
            _gpuCapToEnqueues.Clear();
            _gpuEpochSkips = 0UL;
            _gpuEpochSkipLogCount = 0;
            _gpuSubmitLogCount = 0;
            _postCaptureDropLogCount = 0;
            _gpuHostedTimings.Clear();
            _gpuHostedTimingMissingOrInvalid = 0L;
        }

        /// <summary>Overflow-guarded usability check for the four delta counters on the 120-byte
        /// V1 wire (the host reader already enforces structBytes=120 + version=1).</summary>
        private static bool GpuDeltaCountersUsable(VRSIMPlugin.GpuPipelineStatsV1 st)
        {
            return st.gpuPairCommitted <= (ulong)long.MaxValue && st.uniqueLiveLayerOk <= (ulong)long.MaxValue &&
                st.gpuPairFailed <= (ulong)long.MaxValue && st.exactPoseRejected <= (ulong)long.MaxValue;
        }

        /// <summary>Seed the native 120-byte-stats delta anchor (gpuPairCommitted /
        /// uniqueLiveLayerOk / gpuPairFailed / exactPoseRejected). An unavailable wire or
        /// out-of-range counters leave the anchor INVALID — deltas then report null with an
        /// explicit status, never an assumed zero.</summary>
        private void AnchorGpuNativeBaseline()
        {
            _gpuBaseValid = false;
            var st = _host.ReadGpuPipelineStats();
            if (st.HasValue && GpuDeltaCountersUsable(st.Value))
            {
                _gpuBaseCommitted = (long)st.Value.gpuPairCommitted;
                _gpuBaseLayerOk = (long)st.Value.uniqueLiveLayerOk;
                _gpuBasePairFailed = (long)st.Value.gpuPairFailed;
                _gpuBasePoseRejected = (long)st.Value.exactPoseRejected;
                _gpuBaseEpoch = st.Value.sessionEpoch;
                _gpuBaseValid = true;
            }
        }

        /// <summary>Telemetry note for ONE successful raw-enqueue acceptance (rc==0 only). The
        /// window starts at the FIRST acceptance and that seed frame is excluded from the
        /// window rate — GPU's OWN seed logic, fully separate from the CPU async window. No
        /// file is written here; see WriteGpuWindow (Tick / StopAndRestore).</summary>
        private void NoteGpuEnqueueSuccess(long captureStartQpc, long acceptedQpc, double capturePairMs)
        {
            if (_gpuWindowStartQpc == 0)
            {
                _gpuWindowStartQpc = acceptedQpc;
                _gpuSeedFrameExcluded = _gpuLastEnqueueQpc == 0;
                AnchorGpuNativeBaseline();
            }
            _gpuLastEnqueueQpc = acceptedQpc;
            if (!double.IsNaN(capturePairMs) && !double.IsInfinity(capturePairMs) && capturePairMs >= 0.0)
                _gpuCapPairs.Add(capturePairMs);
            _gpuCapToEnqueues.Add((acceptedQpc - captureStartQpc) * 1000.0 / Stopwatch.Frequency);
            TrimAsyncWindowList(_gpuCapPairs);
            TrimAsyncWindowList(_gpuCapToEnqueues);
            _gpuEnqueueCount++;
        }

        /// <summary>Write the GPU-pipeline telemetry window (periodic ≥10 s, or the single
        /// final flush) to gpu-pipeline-10s.json — ACTUAL raw-enqueue acceptances plus
        /// monotonic deltas of the native 120-byte V1 counters only. Rates are NEVER called a
        /// runtime 120Hz content FPS, and native lastGpuDispatchQpcTicks is a CPU-side enqueue
        /// stamp, NOT a GPU duration (managed CPU-resample and GPU-duration fields report null
        /// by contract). Unavailable or regressed counters report null with an explicit
        /// nativeStatus and re-anchor — never an assumed zero. Nothing here is grafted into
        /// async-pipeline-10s.json.</summary>
        private void WriteGpuWindow(long nowQpc, bool final)
        {
            if (!_gpuPipeline) return;
            if (_gpuWindowStartQpc == 0 || _gpuFinalWritten) return;
            long elapsedQpc = nowQpc - _gpuWindowStartQpc;
            if (!final && elapsedQpc < AsyncWindowQpc) return;   // same 10 s QPC length, fully separate state
            double elapsedMs = elapsedQpc * 1000.0 / Stopwatch.Frequency;
            double elapsedSec = elapsedMs / 1000.0;
            // FIRST anchored window: the seed acceptance (window start) is not window throughput.
            long windowEnqueues = _gpuSeedFrameExcluded
                ? Math.Max(0L, _gpuEnqueueCount - 1L) : (long)_gpuEnqueueCount;
            double enqueueRate = windowEnqueues / elapsedSec;

            long? committedDelta = null, layerOkDelta = null, pairFailedDelta = null, poseRejectedDelta = null;
            string nativeStatus;
            var st = _host.ReadGpuPipelineStats();
            bool curOk = st.HasValue && GpuDeltaCountersUsable(st.Value);
            if (!curOk) nativeStatus = "native_stats_unavailable";
            else if (!_gpuBaseValid)
            {
                nativeStatus = "native_baseline_unavailable_reanchor";
                AnchorGpuNativeBaseline();
            }
            else if (st.Value.sessionEpoch != _gpuBaseEpoch)
            {
                nativeStatus = "native_session_epoch_changed_reanchor";
                AnchorGpuNativeBaseline();   // host restarted mid-window; this window stays unknown
            }
            else if ((long)st.Value.gpuPairCommitted < _gpuBaseCommitted ||
                (long)st.Value.uniqueLiveLayerOk < _gpuBaseLayerOk ||
                (long)st.Value.gpuPairFailed < _gpuBasePairFailed ||
                (long)st.Value.exactPoseRejected < _gpuBasePoseRejected)
            {
                nativeStatus = "native_counter_regression_reanchor";
                AnchorGpuNativeBaseline();   // re-anchor; this window stays unknown
            }
            else
            {
                committedDelta = (long)st.Value.gpuPairCommitted - _gpuBaseCommitted;
                layerOkDelta = (long)st.Value.uniqueLiveLayerOk - _gpuBaseLayerOk;
                pairFailedDelta = (long)st.Value.gpuPairFailed - _gpuBasePairFailed;
                poseRejectedDelta = (long)st.Value.exactPoseRejected - _gpuBasePoseRejected;
                nativeStatus = "ok";
            }
            double? layerDistinctFps = layerOkDelta.HasValue && elapsedSec > 0.0
                ? (double?)((double)layerOkDelta.Value / elapsedSec) : null;

            var sb = new StringBuilder();
            sb.Append("{\"schema\":\"gpu-pipeline-10s/1\"")
              .Append(",\"scope\":\"actual_raw_enqueue_and_runtime_distinct_layer_not_user_perception\"")
              .Append(",\"kind\":").Append(final ? "\"final\"" : "\"periodic_10s\"")
              .Append(",\"clock\":\"Stopwatch QPC (monotonic)\"")
              .Append(",\"generation\":").Append(_generation)
              .Append(",\"selected\":{\"profile\":\"").Append(CaptureProfile.Selected)
              .Append("\",\"width\":").Append(FrameW)
              .Append(",\"height\":").Append(FrameH)
              .Append(",\"nominalTickMs\":").Append(JsonNumber(_nominalTickMs))
              .Append(",\"pipeline\":\"").Append(ProjectionPipelineForMonitor).Append("\"}")
              .Append(",\"elapsedMs\":").Append(JsonNumber(elapsedMs))
              .Append(",\"windowRawEnqueueAccepted\":").Append(_gpuEnqueueCount)
              .Append(",\"windowSeedFrameExcluded\":").Append(_gpuSeedFrameExcluded ? "true" : "false")
              .Append(",\"cumulativeLiveSubmitted\":").Append(_liveSubmitted)
              .Append(",\"cumulativeAllSuccess\":").Append(_framesSubmitted)
              .Append(",\"windowRawEnqueueAcceptedRatePerSec\":").Append(JsonNumber(enqueueRate))
              .Append(",\"fpsNote\":\"measured enqueue-acceptance and distinct-live-layer delta rates; NO runtime 120Hz content-FPS claim\"")
              .Append(",\"nativeDeltas\":{\"gpuPairCommitted\":").Append(JsonLong(committedDelta))
              .Append(",\"uniqueLiveLayerOk\":").Append(JsonLong(layerOkDelta))
              .Append(",\"uniqueLiveLayerOkDistinctFps\":").Append(JsonNumber(layerDistinctFps))
              .Append(",\"uniqueLiveLayerNote\":\"distinct runtime layer commits observed at xrEndFrame from coherent native stats; independently verifiable, NOT user-perceived frames\"")
              .Append(",\"gpuPairFailed\":").Append(JsonLong(pairFailedDelta))
              .Append(",\"exactPoseRejected\":").Append(JsonLong(poseRejectedDelta))
              .Append(",\"nativeStatus\":\"").Append(nativeStatus).Append("\"}");
            if (curOk)
            {
                var s = st.Value;
                var ci = CultureInfo.InvariantCulture;
                sb.Append(",\"native\":{\"sessionEpoch\":").Append(s.sessionEpoch.ToString(ci))
                  .Append(",\"activeFrameSerial\":").Append(s.activeFrameSerial.ToString(ci))
                  .Append(",\"activePoseSerial\":").Append(s.activePoseSerial.ToString(ci))
                  .Append(",\"activeSource\":").Append(s.activeSource.ToString(ci))
                  .Append(",\"activeWidth\":").Append(s.activeWidth.ToString(ci))
                  .Append(",\"activeHeight\":").Append(s.activeHeight.ToString(ci))
                  .Append(",\"lastSuccessfulLayerFrameSerial\":").Append(s.lastSuccessfulLayerFrameSerial.ToString(ci))
                  .Append(",\"lastSuccessfulLayerPoseSerial\":").Append(s.lastSuccessfulLayerPoseSerial.ToString(ci))
                  .Append(",\"rawAdmissionOpen\":").Append((s.reserved & 1u) != 0u ? "true" : "false")
                  .Append(",\"lastGpuDispatchQpcTicks\":").Append(s.lastGpuDispatchQpcTicks.ToString(ci))
                  .Append(",\"lastGpuDispatchNote\":\"native lastGpuDispatchQpcTicks is the CPU-side enqueue duration, NOT a GPU duration\"}");
            }
            else
            {
                sb.Append(",\"native\":null")
                  .Append(",\"nativeUnknownNote\":\"120-byte stats wire unavailable or counters out of range; unknown reported explicitly, never assumed zero\"");
            }
            sb.Append(",\"cpuResampleTimingsMs\":null")
              .Append(",\"gpuDispatchDurationMs\":null")
              .Append(",\"counters\":{\"epochSkips\":").Append(_gpuEpochSkips.ToString(CultureInfo.InvariantCulture))
              .Append(",\"submitFailures\":").Append(_submitFailures)
              .Append(",\"postCaptureDropLogCount\":").Append(_postCaptureDropLogCount).Append('}')
              .Append(",\"stopStatus\":\"").Append(_stopStatus).Append('"')
              .Append(",\"windowListCap\":300");
            AppendPercentiles(sb, "capturePairMs", _gpuCapPairs);
            AppendPercentiles(sb, "captureToEnqueueMs", _gpuCapToEnqueues);
            AppendHostedCaptureTimings(sb);
            sb.Append(",\"capturedUtc\":\"").Append(DateTime.UtcNow.ToString("o")).Append("\"}");
            WriteJson("gpu-pipeline-10s.json", sb.ToString());

            if (final) { _gpuFinalWritten = true; return; }
            _gpuCapPairs.Clear();
            _gpuCapToEnqueues.Clear();
            _gpuHostedTimings.Clear();
            _gpuHostedTimingMissingOrInvalid = 0L;
            _gpuEnqueueCount = 0;
            _gpuWindowStartQpc = nowQpc;   // next window starts at this write boundary
            _gpuSeedFrameExcluded = false; // boundary-anchored window: no seed subtraction
            AnchorGpuNativeBaseline();     // re-anchor native counters at the SAME boundary
        }

        /// <summary>Additive per-window breakdown of ACCEPTED hosted LIVE GPU pairs only, reusing
        /// the owner's existing NC01CaptureSampleV1 managed timings — CPU wall-clock/blocked
        /// durations, NOT GPU dispatch execution time and NOT an FPS claim. Columns reuse
        /// AppendPercentiles (finite values only, per-column n; no samples → null). Pure numeric
        /// projection over the bounded window list: no per-frame IO/JSON/sort/reflection and it
        /// must never throw into the capture path.</summary>
        private void AppendHostedCaptureTimings(StringBuilder sb)
        {
            sb.Append(",\"hostedCaptureTimings\":{\"scope\":\"accepted_hosted_live_gpu_pairs_only_cpu_wallclock_not_gpu_dispatch_duration_not_fps\"")
              .Append(",\"sampleCap\":300")
              .Append(",\"selectedScheduler\":\"").Append(_coalescedBackgroundScheduler ? "timer_plus_coalesced_pulse" : "timer_default").Append('"')
              .Append(",\"sampleCount\":").Append(_gpuHostedTimings.Count)
              .Append(",\"missingOrInvalidAcceptedTimings\":").Append(_gpuHostedTimingMissingOrInvalid)
              .Append(",\"stopwatchFrequency\":").Append(Stopwatch.Frequency);
            var col = new System.Collections.Generic.List<double>(_gpuHostedTimings.Count);
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].PairPrepareMs);
            AppendPercentiles(sb, "pairPrepareMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].PairFinalizeMs);
            AppendPercentiles(sb, "pairFinalizeMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].EyeCameraSumMs);
            AppendPercentiles(sb, "eyeCameraSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].EyeLockWaitSumMs);
            AppendPercentiles(sb, "eyeLockWaitSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].EyeSurfaceRenderSumMs);
            AppendPercentiles(sb, "eyeSurfaceRenderSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].EyeStageValidationSumMs);
            AppendPercentiles(sb, "eyeStageValidationSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].EyeAllocateSumMs);
            AppendPercentiles(sb, "eyeAllocateSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].EyeReadbackCallSumMs);
            AppendPercentiles(sb, "eyeReadbackCallSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].EyeUnlockSumMs);
            AppendPercentiles(sb, "eyeUnlockSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].NativeReadbackSumMs);
            AppendPercentiles(sb, "nativeReadbackSumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++) col.Add(_gpuHostedTimings[i].NativeCopySumMs);
            AppendPercentiles(sb, "nativeCopySumMs", col); col.Clear();
            for (int i = 0; i < _gpuHostedTimings.Count; i++)
            {
                var t = _gpuHostedTimings[i];
                col.Add(t.CapturePairMs - (t.PairPrepareMs + t.PairFinalizeMs + t.EyeCameraSumMs + t.EyeLockWaitSumMs +
                    t.EyeSurfaceRenderSumMs + t.EyeStageValidationSumMs + t.EyeAllocateSumMs +
                    t.EyeReadbackCallSumMs + t.EyeUnlockSumMs));
            }
            AppendPercentiles(sb, "unaccountedPairMs", col);
            sb.Append(",\"overlapNote\":\"pairPrepareMs/pairFinalizeMs are shared per pair and counted once (never eye-summed); the eye...SumMs columns are left+right; nativeReadbackSumMs/nativeCopySumMs are SUBINTERVALS of eyeReadbackCallSumMs and are never re-added into any total; unaccountedPairMs is the signed residual of the same tick's existing capturePairMs minus pairPrepare/pairFinalize and the seven eye sums — it covers post-readback/post-gate/managed-validation/call-boundary untimed CPU work, is NOT GPU time and NOT a completeness guarantee; non-finite values are dropped per column and surface via n\"}");
        }

        // Pure numeric check over CapturePairMs plus the 11 hosted timing columns; no Stopwatch,
        // no SDK, no IO. Signed values are allowed; only NaN/Infinity count as invalid.
        private static bool HasFiniteHostedTiming(HostedPairTiming t)
        {
            return IsFiniteMs(t.CapturePairMs) && IsFiniteMs(t.PairPrepareMs) && IsFiniteMs(t.PairFinalizeMs)
                && IsFiniteMs(t.EyeCameraSumMs) && IsFiniteMs(t.EyeLockWaitSumMs)
                && IsFiniteMs(t.EyeSurfaceRenderSumMs) && IsFiniteMs(t.EyeStageValidationSumMs)
                && IsFiniteMs(t.EyeAllocateSumMs) && IsFiniteMs(t.EyeReadbackCallSumMs)
                && IsFiniteMs(t.EyeUnlockSumMs) && IsFiniteMs(t.NativeReadbackSumMs)
                && IsFiniteMs(t.NativeCopySumMs);
        }

        private static bool IsFiniteMs(double v)
        {
            return !(double.IsNaN(v) || double.IsInfinity(v));
        }

        private static string JsonNumber(double v)
        {
            if (double.IsNaN(v) || double.IsInfinity(v)) return "null";
            return v.ToString("0.00", CultureInfo.InvariantCulture);
        }
        private static string JsonNumber(double? v)
        {
            return v.HasValue ? JsonNumber(v.Value) : "null";
        }
        private static string JsonLong(long? v)
        {
            return v.HasValue ? v.Value.ToString(CultureInfo.InvariantCulture) : "null";
        }

        /// <summary>Per-column p50/p95/max over FINITE samples only, with the actual sample count
        /// reported; percentiles must never be summed across columns. Empty column → null.</summary>
        private static void AppendPercentiles(StringBuilder sb, string name,
            System.Collections.Generic.List<double> values)
        {
            var v = new System.Collections.Generic.List<double>(values.Count);
            for (int i = 0; i < values.Count; i++)
            {
                double d = values[i];
                if (!double.IsNaN(d) && !double.IsInfinity(d)) v.Add(d);
            }
            sb.Append(",\"").Append(name).Append("\":");
            if (v.Count == 0) { sb.Append("null"); return; }
            v.Sort();
            int n = v.Count;
            sb.Append("{\"p50\":").Append(JsonNumber(v[Math.Min(n - 1, (int)(n * 0.50))]))
              .Append(",\"p95\":").Append(JsonNumber(v[Math.Min(n - 1, (int)(n * 0.95))]))
              .Append(",\"max\":").Append(JsonNumber(v[n - 1]))
              .Append(",\"n\":").Append(n).Append('}');
        }

        // ---------------- per-tick: poll completion → latest pose → stereo capture → enqueue ----------------
        private void Tick()
        {
            // Recursive Tick (SDK capture pumping UI callbacks) would clobber _poseSerialUsed
            // before the outer metadata store; NativeCaptureOwner only rejects the capture itself.
            if (_tickInProgress) return;
            _tickInProgress = true;
            bool fromPulse = _coalescedPulseOrigin;
            _coalescedPulseOrigin = false;
            ulong liveBefore = _liveSubmitted;
            try { TickCore(); }
            finally
            {
                _tickInProgress = false;
                // OBS-SHARED (Card3B1): _tickInProgress drops BEFORE the completion helpers below,
                // so this finally-active marker covers the WHOLE remaining finally — a shared stop
                // step never Discards/Finalizes/Completes while any part of this tick is still on
                // the stack. Cleared in a nested finally so no path can leave it stuck true.
                if (_obsSharedRequested) _obsSharedFinallyActive = true;
                try
                {
                    // Hosted PENDING stop: this tick's entered SDK transaction has now exited — attempt
                    // the PURE managed completion (no SDK getter, no capture/cache/submit).
                    if (_hostedStopPending) TryCompleteHostedStopAfterTick();
                    // Hosted failure notification: the SDK transaction is exited here too, so a queued
                    // host report never re-enters the owner mid-capture.
                    TryQueueHostedFailureNotification();
                    // Coalesced-background opt-in: the FIRST pulse is queued ONLY here — after
                    // _tickInProgress=false and stop/notify handling, NEVER mid-TickCore — and only
                    // when this timer-origin tick ACTUALLY accepted a live submit. The retained timer
                    // keeps running either way; accepted-only is scheduling evidence, not an FPS claim.
                    if (_coalescedBackgroundScheduler && !fromPulse && _liveSubmitted != liveBefore)
                        TryScheduleHostedPulseFromTimer();
                }
                finally
                {
                    if (_obsSharedRequested) _obsSharedFinallyActive = false;
                }
            }
        }

        // NC02 XR staging collector — LOCAL CARD ONLY. Exactly one owner poll per UI tick,
        // bound to the SAME original generation/owner, gated on the OLD ticket's
        // row.Identity.CaptureBeginQpc. Does NOT call SubmitRawStereo / ReadGpuPipelineStats /
        // TryGetViews / BuildPlan / a second native source read. Does NOT loop, sleep, retry,
        // or schedule a background poll. Completed result and metadata row are per-turn `out`
        // only — never attached to any global cache and never retaining an SDK wrapper. The
        // existing Stop / Start / Tick / TickCore / RecordHostedCaptureFailure gates are
        // unchanged and are NOT rewired here; a later card will hook this up.
        private bool TryCollectNc02StagingPair(NC04.NC01HostedSceneOwner owner, int originalGeneration,
            out NC02XrQA.NC02SourcePairResultV1 completed, out Nc02XrCaptureMetaRow metadata, out string reason)
        {
            completed = null;
            metadata = null;
            reason = null;

            // 1) strict UI-thread / entry-locked / hosted+gpu / same-owner / running / same-generation
            if (owner == null) return false;
            if (!_nc02StagingRequested) return false;
            if (_hostedOwner == null) return false;
            if (_disp == null || !_disp.CheckAccess()) return false;
            if (_cal == null || _cal.State != EntryCalState.EntryLocked) return false;
            if (!ReferenceEquals(owner, _hostedOwner)) return false;
            if (!_running) return false;
            if (originalGeneration <= 0) return false;
            if (_generation != originalGeneration) return false;
            if (_hostedStartGeneration != originalGeneration) return false;
            if (owner.BoundGeneration != originalGeneration) return false;
            if (!owner.AdmissionOpen) return false;
            if (owner.CaptureBusy) return false;

            // 2) pure numeric scan of the two bounded maps + dict + owner pending count
            int pendingMap = 0;
            int pendingSlotIdx = -1;
            Nc02XrCaptureMetaRow pendingRow = null;
            ulong seenPair0 = 0UL, seenPair1 = 0UL;
            ulong seenFrame0 = 0UL, seenFrame1 = 0UL;
            int seenCount = 0;
            for (int i = 0; i < 2; i++)
            {
                ulong pid = _nc02MapPairIds[i];
                ulong fs = _nc02MapFrameSerials[i];
                if (pid == 0UL && fs == 0UL) continue;
                if (pid == 0UL || fs == 0UL)
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration)
                        RecordHostedCaptureFailure(originalGeneration, "nc02_collect_maps_half_empty");
                    return false;
                }
                Nc02XrCaptureMetaRow row;
                if (!_nc02CaptureRows.TryGetValue(fs, out row) || row == null)
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration)
                        RecordHostedCaptureFailure(originalGeneration, "nc02_collect_maps_orphan");
                    return false;
                }
                if (row.Identity == null || row.Identity.FrameSerial != fs)
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration)
                        RecordHostedCaptureFailure(originalGeneration, "nc02_collect_maps_row_ident_mismatch");
                    return false;
                }
                if (seenCount == 0) { seenPair0 = pid; seenFrame0 = fs; }
                else if (seenCount == 1) { seenPair1 = pid; seenFrame1 = fs; }
                seenCount++;
                pendingMap++;
                pendingSlotIdx = i;
                pendingRow = row;
            }
            if (seenCount >= 2 && (seenPair0 == seenPair1 || seenFrame0 == seenFrame1))
            {
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration)
                    RecordHostedCaptureFailure(originalGeneration, "nc02_collect_maps_duplicate");
                return false;
            }
            if (pendingMap != _nc02CaptureRows.Count)
            {
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration)
                    RecordHostedCaptureFailure(originalGeneration, "nc02_collect_maps_dict_count_mismatch");
                return false;
            }
            if (pendingMap != owner.StagingPendingCount)
            {
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration)
                    RecordHostedCaptureFailure(originalGeneration, "nc02_collect_maps_owner_pending_mismatch");
                return false;
            }
            if (pendingMap > _nc02ManagedPendingLimit)
            {
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration)
                    RecordHostedCaptureFailure(originalGeneration, "nc02_collect_maps_over_limit");
                return false;
            }
            // Normal empty case — nothing pending. Do NOT call owner, do NOT enqueue, do NOT
            // read camera/pose, keep maps/rows intact for the existing verified Stop drain.
            if (pendingMap == 0) return false;
            // pendingLimit==1 and every consistency check above passed → pendingSlotIdx/pendingRow
            // refer to the unique old ticket.
            if (pendingRow == null || pendingSlotIdx < 0)
            {
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration)
                    RecordHostedCaptureFailure(originalGeneration, "nc02_collect_pending_slot_missing");
                return false;
            }

            // 3) pre-call budget bound to the OLD row.Identity.CaptureBeginQpc.
            //    Time compare uses positive Frequency, positive QPC and an ordered long diff;
            //    no long*1000 multiplication (overflow risk). freq/2 == 500 ms in QPC units.
            long freqL = System.Diagnostics.Stopwatch.Frequency;
            int pollBefore = _nc02PollCounts[pendingSlotIdx];
            long oldCaptureBeginQpc = pendingRow.Identity.CaptureBeginQpc;
            long preNowQpc = System.Diagnostics.Stopwatch.GetTimestamp();
            // Mandatory clock sanity: no fake freq=1, no negative poll count, no
            // pre-call timestamp earlier than the OLD row's CaptureBeginQpc.
            if (freqL <= 0L || oldCaptureBeginQpc <= 0L ||
                preNowQpc < oldCaptureBeginQpc || pollBefore < 0)
            {
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_clock_invalid");
                return false;
            }
            long ageGateQpc = freqL / 2L; // 500 ms
            long elapsedPreQpc = preNowQpc - oldCaptureBeginQpc; // ordered long diff
            bool prePollBudget = pollBefore >= 32;
            bool preAgeBudget = elapsedPreQpc >= ageGateQpc;
            if (prePollBudget || preAgeBudget)
            {
                RecordHostedCaptureFailure(originalGeneration,
                    prePollBudget ? "nc02_collect_pending_budget:poll"
                                  : "nc02_collect_pending_budget:age");
                // keep rows/maps/owner for the existing verified Stop drain; do NOT reset
                // old CaptureBeginQpc, do NOT extend the age window, do NOT poll again this turn.
                return false;
            }
            // Increment the slot poll counter BEFORE the actual attempt so the ceiling is
            // exactly 32 real polls (not a 33rd allowance).
            _nc02PollCounts[pendingSlotIdx] = pollBefore + 1;

            NC02XrQA.NC02SourcePairResultV1 result = null;
            string ownerReason = null;
            bool ok = false;
            try
            {
                ok = owner.TryCollectStagingRawPair(originalGeneration, out result, out ownerReason);
            }
            catch (Exception ex)
            {
                // Never let an exception escape into the DispatcherTimer. No rethrow, no
                // SDKStop/Destroy/re-Collect here. Rows/maps/PollCounts stay intact either
                // way; the existing verified Stop drain owns cleanup.
                if (ReferenceEquals(owner, _hostedOwner) && _running &&
                    _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration)
                {
                    RecordHostedCaptureFailure(originalGeneration,
                        "nc02_collect_exception:" + ex.GetType().Name + ":" + ex.Message);
                }
                return false;
            }

            // 3b) post-call gate: Stop / generation change → silent false, keep maps/rows intact.
            if (!ReferenceEquals(owner, _hostedOwner) ||
                _generation != originalGeneration ||
                _hostedStartGeneration != originalGeneration ||
                !_running)
            {
                return false;
            }
            if (!owner.AdmissionOpen)
            {
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_admission_closed_after_call");
                return false;
            }

            if (!ok)
            {
                string r = ownerReason ?? string.Empty;
                if (r.StartsWith("NC02_STAGING_COLLECT_PENDING:", System.StringComparison.Ordinal))
                {
                    // aggregate pure-number counter (NOT an FPS contribution; slot PollCounts
                    // already advanced before the call — do NOT double-increment that).
                    _nc02PendingPolls++;
                    long postNowQpc = System.Diagnostics.Stopwatch.GetTimestamp();
                    if (postNowQpc < oldCaptureBeginQpc)
                    {
                        RecordHostedCaptureFailure(originalGeneration, "nc02_collect_clock_invalid");
                        return false;
                    }
                    long elapsedPostQpc = postNowQpc - oldCaptureBeginQpc;
                    int pollNow = _nc02PollCounts[pendingSlotIdx];
                    if (pollNow >= 32 || elapsedPostQpc >= ageGateQpc)
                    {
                        RecordHostedCaptureFailure(originalGeneration,
                            pollNow >= 32 ? "nc02_collect_pending_budget:poll_after"
                                          : "nc02_collect_pending_budget:age_after");
                        return false;
                    }
                    // normal Pending — no enqueue, no camera/pose read, no fallback, no fake success.
                    reason = r;
                    return false;
                }
                // Anomaly / fatal reason (CallBusy / NativeFault / TokenMismatch / etc.) — never
                // silent-pend. Still same-gen alive (checked above) → controlled failure entry.
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_owner_reason:" + r);
                return false;
            }

            // 5) success: locate the exact slot by result.PairId, then mapFrame → row.
            if (result == null)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_null_result");
                return false;
            }
            int foundSlot = -1;
            for (int i = 0; i < 2; i++)
            {
                if (_nc02MapPairIds[i] != 0UL && _nc02MapFrameSerials[i] != 0UL &&
                    _nc02MapPairIds[i] == result.PairId)
                {
                    if (foundSlot >= 0)
                    {
                        _nc02Dropped++;
                        RecordHostedCaptureFailure(originalGeneration, "nc02_collect_dup_pair_slot");
                        return false;
                    }
                    foundSlot = i;
                }
            }
            if (foundSlot < 0)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_result_pair_unmapped");
                return false;
            }
            ulong mapFrameSerial = _nc02MapFrameSerials[foundSlot];
            if (result.FrameSerial != mapFrameSerial)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_result_frame_mismatch");
                return false;
            }
            Nc02XrCaptureMetaRow oldRow;
            if (!_nc02CaptureRows.TryGetValue(mapFrameSerial, out oldRow) || oldRow == null)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_result_row_missing");
                return false;
            }
            string matchFail = oldRow.MatchesCollected(result);
            if (matchFail != null)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_match:" + matchFail);
                return false;
            }

            // 6) re-verify (still same gen/owner/running/admission, and not busy) BEFORE commit.
            if (!_running || _generation != originalGeneration ||
                _hostedStartGeneration != originalGeneration ||
                !ReferenceEquals(owner, _hostedOwner) ||
                !owner.AdmissionOpen || owner.CaptureBusy)
            {
                // Stop landed mid-matching — do NOT publish; retain maps/rows for verified drain.
                return false;
            }

            // 6b) MANDATORY pure-numeric CollectQpc validation BEFORE any commit.
            // Latency ONLY from the SAME row.Identity.CaptureBeginQpc → result.CollectQpc,
            // in double math (no long * 1000 overflow). Any violation is a controlled drop
            // that keeps ALL old rows/maps/PollCounts intact for the verified Stop drain.
            ulong collectQpcU = result.CollectQpc;
            if (collectQpcU == 0UL || collectQpcU > (ulong)long.MaxValue)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_qpc_invalid");
                return false;
            }
            long collectQpcL = (long)collectQpcU;
            long beginQpc = oldRow.Identity.CaptureBeginQpc;
            long qpcFreqL = System.Diagnostics.Stopwatch.Frequency;
            if (beginQpc <= 0L || qpcFreqL <= 0L || collectQpcL < beginQpc)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_qpc_invalid");
                return false;
            }
            double latencyMs = (double)(collectQpcL - beginQpc) * 1000.0 / (double)qpcFreqL;
            if (double.IsNaN(latencyMs) || double.IsInfinity(latencyMs) || latencyMs < 0.0)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_qpc_invalid");
                return false;
            }

            // Pure-numeric raw-matrix delta diagnostic commit, BEFORE map/row cleanup and only
            // after all existing match / generation / CollectQpc / latency gates succeeded.
            double rawDelta = oldRow.LastRawMatrixMaxAbsDelta;
            if (double.IsNaN(rawDelta) || double.IsInfinity(rawDelta) || rawDelta < 0.0 || rawDelta > 1e-6)
            {
                _nc02Dropped++;
                RecordHostedCaptureFailure(originalGeneration, "nc02_collect_raw_delta_invalid");
                return false;
            }
            _nc02LastRawMatrixMaxAbsDelta = rawDelta;
            if (double.IsNaN(_nc02MaxRawMatrixAbsDelta) || rawDelta > _nc02MaxRawMatrixAbsDelta)
                _nc02MaxRawMatrixAbsDelta = rawDelta;

            // Remove ONLY this FrameSerial dict entry and clear ONLY this map slot / FrameSerial /
            // PollCount (NOT a full clear).
            _nc02CaptureRows.Remove(mapFrameSerial);
            _nc02MapPairIds[foundSlot] = 0UL;
            _nc02MapFrameSerials[foundSlot] = 0UL;
            _nc02PollCounts[foundSlot] = 0;

            _nc02Collected++;
            _nc02LastSourceLatencyMs = latencyMs;
            // _nc02LastPoseToEnqueueMs intentionally NOT touched on this card.

            // NC02 cost diagnostics — this validated commit is the ONLY feed (actual collected pairs;
            // never pending / rejected / mismatched / native-enqueue attempts). Numeric result fields
            // and the raw176 THIS-pair windows only; conversion faults increment invalid, never escape
            // and never affect publication. No SDK/native query runs here.
            bool nc02DiagObservedCounted = false, nc02DiagOutcomeCounted = false;
            try
            {
                long dfreqL = System.Diagnostics.Stopwatch.Frequency;
                byte[] draw = result.NativeCollectedRawBytes;
                bool dok = dfreqL > 0L && System.BitConverter.IsLittleEndian &&
                    result.NativeCollectedBytes == 176 && draw != null && draw.Length == 176 &&
                    oldRow.Identity != null;
                double dSubmitMs = double.NaN, dCollectMs = double.NaN, dEyeLockMs = double.NaN,
                    dRenderMs = double.NaN, dPrepareMs = double.NaN, dFinalizeMs = double.NaN;
                ulong dGrtTicks = 0UL, dIssueTicks = 0UL, dQueryTicks = 0UL, dLockTicks = 0UL,
                    dRowCopyTicks = 0UL, dUnlockTicks = 0UL, dSubmitTotalTicks = 0UL, dPollTotalTicks = 0UL;
                ulong dNativeSubmitQpc = 0UL, dNativeCollectQpc = 0UL;
                long dCaptureBeginQpc = 0L, dCaptureEndQpc = 0L;
                ulong dQueryAttempts = 0UL, dLeftLockAttempts = 0UL, dRightLockAttempts = 0UL;
                if (dok)
                {
                    // Raw 176-byte THIS-pair token header validation BEFORE any decode/publication.
                    // The already-MatchesCollected pair stands regardless; a failure here only marks
                    // this diagnostic record unusable (invalid) — never a capture fault.
                    dok =
                        System.BitConverter.ToUInt32(draw, 0) == 40U &&
                        System.BitConverter.ToUInt32(draw, 4) == 1U &&
                        result.PairId > 0UL &&
                        System.BitConverter.ToUInt64(draw, 8) == result.PairId &&
                        System.BitConverter.ToUInt64(draw, 16) == oldRow.Identity.PoseSerial &&
                        System.BitConverter.ToUInt64(draw, 24) == oldRow.Identity.NativeXrEpoch &&
                        System.BitConverter.ToUInt64(draw, 32) == (ulong)originalGeneration &&
                        oldRow.Identity.OriginalGeneration == originalGeneration &&
                        System.BitConverter.ToUInt32(draw, 44) == 0U &&
                        System.BitConverter.ToUInt32(draw, 72) == oldRow.Width &&
                        System.BitConverter.ToUInt32(draw, 76) == oldRow.Height &&
                        System.BitConverter.ToUInt32(draw, 172) == 0U;
                }
                if (dok)
                {
                    dSubmitMs = result.SubmitNativeMs;   // includes the FIRST Create when this pair bootstrapped
                    dCollectMs = result.CollectNativeMs; // includes every poll of this OLD ticket
                    dEyeLockMs = result.EyeLockMs;
                    dRenderMs = result.SurfaceRenderMs;
                    dPrepareMs = result.PairPrepareMs;
                    dFinalizeMs = result.PairFinalizeMs;
                    dok =
                        !double.IsNaN(dSubmitMs) && !double.IsInfinity(dSubmitMs) && dSubmitMs >= 0.0 &&
                        !double.IsNaN(dCollectMs) && !double.IsInfinity(dCollectMs) && dCollectMs >= 0.0 &&
                        !double.IsNaN(dEyeLockMs) && !double.IsInfinity(dEyeLockMs) && dEyeLockMs >= 0.0 &&
                        !double.IsNaN(dRenderMs) && !double.IsInfinity(dRenderMs) && dRenderMs >= 0.0 &&
                        !double.IsNaN(dPrepareMs) && !double.IsInfinity(dPrepareMs) && dPrepareMs >= 0.0 &&
                        !double.IsNaN(dFinalizeMs) && !double.IsInfinity(dFinalizeMs) && dFinalizeMs >= 0.0;
                }
                if (dok)
                {
                    dGrtTicks = System.BitConverter.ToUInt64(draw, 96);
                    dIssueTicks = System.BitConverter.ToUInt64(draw, 104);
                    dQueryTicks = System.BitConverter.ToUInt64(draw, 112);
                    dLockTicks = System.BitConverter.ToUInt64(draw, 120);
                    dRowCopyTicks = System.BitConverter.ToUInt64(draw, 128);
                    dUnlockTicks = System.BitConverter.ToUInt64(draw, 136);
                    dSubmitTotalTicks = System.BitConverter.ToUInt64(draw, 144);
                    dPollTotalTicks = System.BitConverter.ToUInt64(draw, 152);
                    dQueryAttempts = (ulong)System.BitConverter.ToUInt32(draw, 160);
                    dLeftLockAttempts = (ulong)System.BitConverter.ToUInt32(draw, 164);
                    dRightLockAttempts = (ulong)System.BitConverter.ToUInt32(draw, 168);
                    dNativeSubmitQpc = System.BitConverter.ToUInt64(draw, 80);
                    dNativeCollectQpc = System.BitConverter.ToUInt64(draw, 88);
                    dCaptureBeginQpc = oldRow.Identity.CaptureBeginQpc;
                    dCaptureEndQpc = result.CaptureEndQpc;
                    // Ordered window checks only — no multiplication / subtraction that could overflow.
                    // An invalid order here only invalidates diagnostics, never the accepted pair.
                    dok =
                        dNativeSubmitQpc > 0UL && dNativeSubmitQpc <= (ulong)long.MaxValue &&
                        dNativeCollectQpc > 0UL && dNativeCollectQpc <= (ulong)long.MaxValue &&
                        dNativeCollectQpc == result.CollectQpc &&
                        dCaptureBeginQpc > 0L && dCaptureEndQpc > 0L &&
                        (long)dNativeSubmitQpc >= dCaptureBeginQpc &&
                        (long)dNativeSubmitQpc <= dCaptureEndQpc &&
                        dCaptureEndQpc >= dCaptureBeginQpc &&
                        (long)dNativeCollectQpc >= dCaptureEndQpc;
                }
                double sSubmit = double.NaN, sCollect = double.NaN, sEyeLock = double.NaN,
                    sRender = double.NaN, sPrepare = double.NaN, sFinalize = double.NaN;
                if (dok)
                {
                    sSubmit = double.IsNaN(_nc02DiagSumSubmitNativeMs) ? dSubmitMs : _nc02DiagSumSubmitNativeMs + dSubmitMs;
                    sCollect = double.IsNaN(_nc02DiagSumCollectNativeMs) ? dCollectMs : _nc02DiagSumCollectNativeMs + dCollectMs;
                    sEyeLock = double.IsNaN(_nc02DiagSumEyeLockMs) ? dEyeLockMs : _nc02DiagSumEyeLockMs + dEyeLockMs;
                    sRender = double.IsNaN(_nc02DiagSumSurfaceRenderMs) ? dRenderMs : _nc02DiagSumSurfaceRenderMs + dRenderMs;
                    sPrepare = double.IsNaN(_nc02DiagSumPairPrepareMs) ? dPrepareMs : _nc02DiagSumPairPrepareMs + dPrepareMs;
                    sFinalize = double.IsNaN(_nc02DiagSumPairFinalizeMs) ? dFinalizeMs : _nc02DiagSumPairFinalizeMs + dFinalizeMs;
                    dok =
                        !double.IsNaN(sSubmit) && !double.IsInfinity(sSubmit) &&
                        !double.IsNaN(sCollect) && !double.IsInfinity(sCollect) &&
                        !double.IsNaN(sEyeLock) && !double.IsInfinity(sEyeLock) &&
                        !double.IsNaN(sRender) && !double.IsInfinity(sRender) &&
                        !double.IsNaN(sPrepare) && !double.IsInfinity(sPrepare) &&
                        !double.IsNaN(sFinalize) && !double.IsInfinity(sFinalize);
                }
                lock (_nc02DiagLock)
                {
                    // Capacity check BEFORE any counter increment — never wrap negative. At capacity
                    // the previous snapshot is retained and diagnostics stop advancing; exactly one
                    // observed plus exactly one valid-OR-invalid outcome per hook below the cap.
                    if (_nc02DiagObservedCollected < long.MaxValue)
                    {
                        _nc02DiagObservedCollected++;
                        nc02DiagObservedCounted = true;
                        if (dok &&
                            _nc02DiagSumQueryAttempts <= ulong.MaxValue - dQueryAttempts &&
                            _nc02DiagSumLeftLockAttempts <= ulong.MaxValue - dLeftLockAttempts &&
                            _nc02DiagSumRightLockAttempts <= ulong.MaxValue - dRightLockAttempts)
                        {
                            if (_nc02DiagValidCommits < long.MaxValue) _nc02DiagValidCommits++;
                            nc02DiagOutcomeCounted = true;
                            _nc02DiagGeneration = originalGeneration;
                            _nc02DiagLastPairId = result.PairId;
                            _nc02DiagLastFrameSerial = result.FrameSerial;
                            _nc02DiagLastPoseSerial = result.NativePoseSerial;
                            _nc02DiagLastNativeXrEpoch = result.NativeXrEpoch;
                            _nc02DiagLastSubmitNativeMs = dSubmitMs;
                            _nc02DiagLastCollectNativeMs = dCollectMs;
                            _nc02DiagLastEyeLockMs = dEyeLockMs;
                            _nc02DiagLastSurfaceRenderMs = dRenderMs;
                            _nc02DiagLastPairPrepareMs = dPrepareMs;
                            _nc02DiagLastPairFinalizeMs = dFinalizeMs;
                            _nc02DiagSumSubmitNativeMs = sSubmit;
                            _nc02DiagSumCollectNativeMs = sCollect;
                            _nc02DiagSumEyeLockMs = sEyeLock;
                            _nc02DiagSumSurfaceRenderMs = sRender;
                            _nc02DiagSumPairPrepareMs = sPrepare;
                            _nc02DiagSumPairFinalizeMs = sFinalize;
                            _nc02DiagLastGetRtdTicks = dGrtTicks;
                            _nc02DiagLastIssueTicks = dIssueTicks;
                            _nc02DiagLastQueryTicks = dQueryTicks;
                            _nc02DiagLastLockTicks = dLockTicks;
                            _nc02DiagLastRowCopyTicks = dRowCopyTicks;
                            _nc02DiagLastUnlockTicks = dUnlockTicks;
                            _nc02DiagLastSubmitTotalTicks = dSubmitTotalTicks;
                            _nc02DiagLastPollTotalTicks = dPollTotalTicks;
                            _nc02DiagLastQueryAttempts = dQueryAttempts;
                            _nc02DiagLastLeftLockAttempts = dLeftLockAttempts;
                            _nc02DiagLastRightLockAttempts = dRightLockAttempts;
                            _nc02DiagSumQueryAttempts += dQueryAttempts;
                            _nc02DiagSumLeftLockAttempts += dLeftLockAttempts;
                            _nc02DiagSumRightLockAttempts += dRightLockAttempts;
                            _nc02DiagLastCaptureBeginQpc = dCaptureBeginQpc;
                            _nc02DiagLastCaptureEndQpc = dCaptureEndQpc;
                            _nc02DiagLastNativeSubmitQpc = dNativeSubmitQpc;
                            _nc02DiagLastNativeCollectQpc = dNativeCollectQpc;
                            _nc02DiagLastKnown = true;
                        }
                        else
                        {
                            // Invalid record: prior good last values/IDs stay untouched.
                            if (_nc02DiagInvalidCommits < long.MaxValue) _nc02DiagInvalidCommits++;
                            nc02DiagOutcomeCounted = true;
                        }
                    }
                }
            }
            catch (System.Exception)
            {
                lock (_nc02DiagLock)
                {
                    if (!nc02DiagObservedCounted && !nc02DiagOutcomeCounted &&
                        _nc02DiagObservedCollected < long.MaxValue)
                    {
                        _nc02DiagObservedCollected++;
                        if (_nc02DiagInvalidCommits < long.MaxValue) _nc02DiagInvalidCommits++;
                    }
                }
            }

            completed = result;
            metadata = oldRow;
            return true;
        }

        // NC02-XR-P5: ONE bounded staging-source capture submit per tick. Synchronous single
        // transaction — no loops, awaits, sleeps or SDK threads. Never touches the static
        // source-epoch/pose-0 API; the ticket identity comes from this tick's real
        // stable-bracket pose window (poseSerial/start/end) and the frozen native epoch.
        private bool TrySubmitNc02StagingCapture(NC04.NC01HostedSceneOwner owner, int originalGeneration,
            Matrix mL, Matrix mR, VRSIMPlugin.VRSIMEyeView leftEye, VRSIMPlugin.VRSIMEyeView rightEye,
            VRSIMPlugin.RawProjectionPlanV1 leftPlan, VRSIMPlugin.RawProjectionPlanV1 rightPlan,
            double sourceFovLeft, double sourceFovRight, ulong poseSerial, ulong nativeXrEpoch,
            long poseSampleStartQpc, long poseSampleEndQpc)
        {
            // ---- PURE pre-gates first — no owner transaction before parameter validation ----
            if (!_nc02StagingRequested || !_running || !_hostedBackendSelected || !_gpuPipeline)
                return false;
            if (!_disp.CheckAccess()) return false;
            if (_cal == null || _cal.State != EntryCalState.EntryLocked) return false;
            if (owner == null || originalGeneration < 0) return false;
            if (_generation != originalGeneration || _hostedStartGeneration != originalGeneration)
                return false;
            if (!ReferenceEquals(_hostedOwner, owner)) return false;
            if (owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen || owner.CaptureBusy)
                return false;
            if (poseSerial == 0UL || nativeXrEpoch == 0UL) return false;
            if (poseSampleStartQpc <= 0L || poseSampleEndQpc < poseSampleStartQpc) return false;

            // managed pending (limit 1) must be fully zero; half-empty/contradictory converges
            var ownerPending = owner.StagingPendingCount;
            int rowCount = _nc02CaptureRows.Count;
            bool mapsNonzero = _nc02MapPairIds[0] != 0UL || _nc02MapPairIds[1] != 0UL ||
                _nc02MapFrameSerials[0] != 0UL || _nc02MapFrameSerials[1] != 0UL;
            if (ownerPending != 0)
            {
                if (rowCount != 0 || mapsNonzero) return false;   // pending full — normal no-op
                RecordHostedCaptureFailure(originalGeneration,
                    "nc02_staging_state_inconsistent pending_without_refs");
                return false;
            }
            if (rowCount != 0 || mapsNonzero)
            {
                RecordHostedCaptureFailure(originalGeneration,
                    "nc02_staging_state_inconsistent refs_without_pending rows=" + rowCount +
                    " mapsNonzero=" + (mapsNonzero ? 1 : 0));
                return false;
            }

            // freeze one clean fixed map slot BEFORE any SDK work
            int slot = -1;
            for (int i = 0; i < 2; i++)
            {
                if (_nc02MapPairIds[i] == 0UL && _nc02MapFrameSerials[i] == 0UL) { slot = i; break; }
            }
            if (slot < 0)
            {
                RecordHostedCaptureFailure(originalGeneration, "nc02_staging_state_inconsistent no_free_map_slot");
                return false;
            }

            // shared _frameSerial reserve (static preview uses the SAME counter) — never a local
            // counter, never a guessed owner PairId. MaxValue rejected before the increment so
            // the reserved value can never wrap to 0 (first tick 0 -> 1 is legal).
            if (_frameSerial == ulong.MaxValue)
            {
                RecordHostedCaptureFailure(originalGeneration, "nc02_frame_serial_exhausted");
                return false;
            }
            ulong reservedFrameSerial = ++_frameSerial;

            long captureBeginQpc = Stopwatch.GetTimestamp();
            var identity = new NC02XrQA.NC02XrCaptureIdentityV1(
                reservedFrameSerial, poseSerial, nativeXrEpoch, originalGeneration,
                poseSampleStartQpc, poseSampleEndQpc, captureBeginQpc);

            // shouldStop closure allocated BEFORE any owner submit, bound to the ORIGINAL refs
            Func<bool> shouldStop = () =>
                !_running ||
                _generation != originalGeneration ||
                _hostedStartGeneration != originalGeneration ||
                !ReferenceEquals(_hostedOwner, owner);

            bool inserted = false;
            try
            {
                // ctor copies every numeric value (private clones); quality 1 is guaranteed by
                // the owner's original Ready + Submit path; matrix order unchanged.
                var row = new Nc02XrCaptureMetaRow(identity, mL, mR, leftPlan, rightPlan,
                    leftEye, rightEye, sourceFovLeft, sourceFovRight,
                    FrameW, FrameH, FrameW * 4, 1, _lastYawDelta,
                    _R_E, _P_E, _head0, _qHead0);

                // the Matrix indexer read in the ctor may pump — re-check PURE gates before Add
                if (!_running || _generation != originalGeneration ||
                    _hostedStartGeneration != originalGeneration || !ReferenceEquals(_hostedOwner, owner) ||
                    owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen || owner.CaptureBusy)
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                        RecordHostedCaptureFailure(originalGeneration, "nc02_staging_preaccept_gate_lost");
                    return false;   // nothing added, nothing submitted
                }

                // row is registered strictly BEFORE the owner submit
                _nc02CaptureRows.Add(identity.FrameSerial, row);
                inserted = true;

                // FINAL admission re-check with the ticket-bound epoch — never replace the identity
                var gst = _host.ReadGpuPipelineStats();
                bool admissionOk = gst.HasValue && gst.Value.reserved == 1u &&
                    gst.Value.sessionEpoch == nativeXrEpoch;
                if (!admissionOk ||
                    !_running || _generation != originalGeneration ||
                    _hostedStartGeneration != originalGeneration || !ReferenceEquals(_hostedOwner, owner) ||
                    owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen || owner.CaptureBusy)
                {
                    // pre-accept convergence; the row may only go if owner provably holds nothing
                    if (owner.StagingPendingCount == 0)
                    {
                        _nc02CaptureRows.Remove(identity.FrameSerial);
                        inserted = false;
                    }
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                        RecordHostedCaptureFailure(originalGeneration, admissionOk
                            ? "nc02_staging_preaccept_gate_lost_after_row"
                            : "nc02_staging_preaccept_admission_or_epoch_invalid nativeEpoch=" + nativeXrEpoch +
                              " currentEpoch=" + (gst.HasValue ? gst.Value.sessionEpoch : 0UL));
                    return false;
                }

                _renderAttempts++;
                ulong acceptedPairId = 0UL;
                string reason = null;
                bool accepted = owner.TrySubmitXrStagingRawPair(mL, mR, sourceFovLeft, sourceFovRight,
                    1.0, true, originalGeneration, shouldStop, identity, out acceptedPairId, out reason);

                if (accepted)
                {
                    // FIRST and only fixed-array writes — before ANY getter, log or status read;
                    // the pairId is the owner's out value, never guessed.
                    _nc02MapPairIds[slot] = acceptedPairId;
                    _nc02MapFrameSerials[slot] = identity.FrameSerial;
                    _nc02PollCounts[slot] = 0;
                    _nc02SubmitAccepted++;
                    if (acceptedPairId == 0UL ||
                        !_running || _generation != originalGeneration ||
                        _hostedStartGeneration != originalGeneration || !ReferenceEquals(_hostedOwner, owner))
                    {
                        // an accepted ticket exists; late stop must NOT remove the row or clear
                        // the maps — the existing drain converges.
                        return false;
                    }
                    return true;   // source pair ACCEPTED only — no display, no content-FPS claim
                }

                // Source Submit returned false: do NOT classify the reason and do NOT clear the
                // prepared row — a "benign"-looking reason could mask a held pending ticket.
                // Keep the row and the owner refs; never guess the PairId, never native-destroy.
                // An explicit Stop that already invalidated the generation converges via its
                // finite drain; here we only record a failure while this generation is current.
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                    RecordHostedCaptureFailure(originalGeneration,
                        "nc02_staging_submit_rejected:" + (string.IsNullOrEmpty(reason) ? "unknown" : reason));
                return false;
            }
            catch (Exception sex)
            {
                bool submitPending = true;
                bool submitAdmissionClosed = true;
                try { submitPending = owner.StagingPendingCount != 0; } catch { }
                try { submitAdmissionClosed = !owner.AdmissionOpen; } catch { }
                // pending or closed admission keeps the row; only a proven-not-accepted,
                // pending-0, open-admission case removes the row this transaction added
                if (inserted && !submitPending && !submitAdmissionClosed)
                {
                    try { _nc02CaptureRows.Remove(identity.FrameSerial); } catch { }
                }
                // parameter/exception records always use the ORIGINAL generation, never the
                // updated _generation
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                    RecordHostedCaptureFailure(originalGeneration,
                        "nc02_staging_transaction_exception " + sex.GetType().Name + ": " + sex.Message);
                return false;
            }
        }
        // ============================================================================================
        // OBS-SHARED typed GPU capture (Card3A). ONE bounded PREPARE→managed-return→COMMIT owner
        // transaction per tick, with exact DISCARD tracking on every failure path. Activation is hard
        // OFF (ManagedObsSharedActivation=false) until Native2B/Managed3B pass, so none of this runs at
        // runtime yet — this card only wires compilable hooks. It NEVER performs a CPU pixel readback,
        // never falls into CaptureStereoPair/ForceAlpha/SubmitRawStereo/StoreCaptureMeta/StorePreview,
        // never closes/duplicates the legacy shared HANDLE numbers, and makes no FPS/display claim.
        // ============================================================================================

        // Read-only cached bridge release. A COMMITTED-but-unread packet may be recycled for a NEW
        // managed ledger entry ONLY when: the frozen stop/stats wire reads both complete, the cached
        // boundEpoch/boundGeneration match THAT retained packet's original epoch/generation (not the
        // current generation), the packet's native slotIndex is in range, and the native bridge reports
        // that slot genuinely FREE. Pending, a runtime frame advance, or a fresh generation are NEVER
        // FREE evidence. No UI poll/EndFrame/Flush, no owner capture, no second source read here.
        private void TryReleaseObsSharedCommittedPackets(int originalGeneration,
            VRSIMPlugin.SharedGpuStopStateV1 stop, bool stopOk,
            VRSIMPlugin.SharedGpuBridgeStatsV1 stats, bool statsOk)
        {
            if (!stopOk || !statsOk) return;
            for (int i = 0; i < _obsSharedTickets.Length; i++)
            {
                // only a COMMITTED (native-owned), NOT locally-owned packet is a recycle candidate
                if (!_obsSharedCommitted[i] || _obsSharedOwned[i]) continue;
                var tk = _obsSharedTickets[i];
                if (tk.ticket == 0UL) continue;
                if (tk.slotIndex >= ObsSharedSlotCountV1) continue;   // malformed slot — never index
                if (originalGeneration <= 0 || tk.generation != (uint)originalGeneration) continue;
                if (stop.boundEpoch != tk.epoch) continue;            // bound must be THIS packet's epoch
                if (stop.boundGeneration != tk.generation) continue;  // ... and generation
                uint slotState = tk.slotIndex == 0u ? stats.slotState0
                    : (tk.slotIndex == 1u ? stats.slotState1 : stats.slotState2);
                if (slotState != ObsSharedBridgeSlotStateFreeV1) continue;   // slot still busy — retain
                // Proven FREE: native consumed the packet; recycle the managed entry for a fresh Start tick.
                _obsSharedCommitted[i] = false;
                _obsSharedTickets[i] = default(VRSIMPlugin.SharedGpuTicketV1);
                _obsSharedIdentities[i] = default(VRSIMPlugin.SharedGpuCaptureIdentityV1);
            }
        }

        // Exact DISCARD of a locally-owned PREPARED packet, using ONLY the stored complete ticket +
        // original identity. Never rewrites the ticket with the current _generation/epoch, never reruns
        // the owner, never native-destroys a foreign handle, never a CPU fallback. Returns null for a
        // CLEAN outcome (no owned packet, or an actual native rc==0 that cleared the entry); returns the
        // REAL discard fault (the original exception, or a synthetic InvalidOperationException naming the
        // rc/ticket and the ORIGINAL identity generation) for Pending/error/quarantine/exception. On any
        // failure the entry is RETAINED (ticket + identity + owned flag untouched); only native rc==0
        // clears it. This helper never Records and never swallows — the caller aggregates the evidence.
        private Exception TryDiscardObsSharedPrepared(int ledgerIndex)
        {
            if (ledgerIndex < 0 || ledgerIndex >= _obsSharedOwned.Length) return null;
            if (!_obsSharedOwned[ledgerIndex]) return null;
            var ticket = _obsSharedTickets[ledgerIndex];
            if (ticket.ticket == 0UL) { _obsSharedOwned[ledgerIndex] = false; return null; }   // nothing to discard

            // The ORIGINAL identity generation locates the fault generation; a possibly-malformed
            // returned ticket.generation is NEVER used to rebind the fault.
            int origGen = (int)_obsSharedIdentities[ledgerIndex].generation;

            Exception discardError = null;
            int drc = (int)VRSIMPlugin.SharedGpuResultV1.Error;
            try { drc = _host.DiscardSharedGpu(ref ticket); }
            catch (Exception dex) { discardError = dex; }

            if (discardError == null && drc == (int)VRSIMPlugin.SharedGpuResultV1.Completed)
            {
                // actual native rc==0 is the ONLY path that releases local ownership + clears the entry.
                _obsSharedOwned[ledgerIndex] = false;
                _obsSharedTickets[ledgerIndex] = default(VRSIMPlugin.SharedGpuTicketV1);
                _obsSharedIdentities[ledgerIndex] = default(VRSIMPlugin.SharedGpuCaptureIdentityV1);
                return null;
            }

            // Discard NOT completed: RETAIN the entry + original packet/identity as fault evidence —
            // never clear the array to fake a clean pass, never touch ticket fields. Return the real
            // fault to the caller for combined aggregation (no Record, no swallow, no increment here).
            if (discardError != null) return discardError;
            return new InvalidOperationException(
                "OBS_SHARED_DISCARD_NOT_COMPLETED rc=" + drc.ToString(CultureInfo.InvariantCulture) +
                " ticket=" + ticket.ticket.ToString(CultureInfo.InvariantCulture) +
                " identityGeneration=" + origGen.ToString(CultureInfo.InvariantCulture));
        }

        // The single typed shared-capture transaction. Returns true ONLY for a native COMMIT rc==0
        // (ownership transferred to the native worker/ring). Every other outcome (normal skip,
        // backpressure, refusal, postguard fault, commit rejection) returns false; the caller (Tick)
        // ends the tick unconditionally either way.
        private bool TrySubmitObsSharedCapture(NC04.NC01HostedSceneOwner owner, int originalGeneration,
            Matrix leftRaw, Matrix rightRaw,
            VRSIMPlugin.RawProjectionPlanV1 leftPlan, VRSIMPlugin.RawProjectionPlanV1 rightPlan,
            double sourceFovLeft, double sourceFovRight, ulong poseSerial, ulong nativeEpoch,
            long poseBeginQpc, long poseEndQpc)
        {
            // ---- PURE pre-gates: no owner transaction before validation ----
            if (!_obsSharedRequested || !_running || !_hostedBackendSelected || !_gpuPipeline)
                return false;
            if (!ManagedObsSharedActivation) return false;   // hard OFF — compilable hooks only
            if (owner == null || originalGeneration <= 0) return false;
            if (_disp == null || !_disp.CheckAccess()) return false;
            if (_cal == null || _cal.State != EntryCalState.EntryLocked) return false;
            if (_generation != originalGeneration || _hostedStartGeneration != originalGeneration)
                return false;
            if (!ReferenceEquals(_hostedOwner, owner)) return false;
            if (owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen ||
                !string.IsNullOrEmpty(owner.FailureReason) || owner.CaptureBusy)
                return false;
            if (!IsCaptureBackendReady) return false;
            if (CaptureProfile.Selected != ObsSharedCaptureProfileV1 ||
                FrameW != ObsSharedTicketEdgeV1 || FrameH != ObsSharedTicketEdgeV1)
                return false;
            if (poseSerial == 0UL || nativeEpoch == 0UL) return false;
            if (poseBeginQpc <= 0L || poseEndQpc < poseBeginQpc) return false;
            if (double.IsNaN(_lastYawDelta) || double.IsInfinity(_lastYawDelta)) return false;

            // frame serial exhaustion — reject BEFORE the increment; reserved numbers never wrap to 0 and
            // a failed number is never reused (the counter advances monotonically like the static preview).
            if (_frameSerial == ulong.MaxValue)
            {
                RecordHostedCaptureFailure(originalGeneration, "obs_shared_frame_serial_exhausted");
                return false;
            }

            // Read-only cached bridge state, then a FREE managed ledger index (native slot need NOT equal
            // the managed index; malformed slots are never indexed). Select only ticket.ticket==0.
            VRSIMPlugin.SharedGpuStopStateV1 stop;
            int stopRc;
            try { stopRc = _host.ReadSharedGpuStop(out stop); } catch { stop = default(VRSIMPlugin.SharedGpuStopStateV1); stopRc = -1; }
            bool stopOk = stopRc == (int)VRSIMPlugin.SharedGpuResultV1.Completed;
            VRSIMPlugin.SharedGpuBridgeStatsV1 stats;
            int statsRc;
            try { statsRc = _host.ReadSharedGpuBridgeStats(out stats); } catch { stats = default(VRSIMPlugin.SharedGpuBridgeStatsV1); statsRc = -1; }
            bool statsOk = statsRc == (int)VRSIMPlugin.SharedGpuResultV1.Completed;
            TryReleaseObsSharedCommittedPackets(originalGeneration, stop, stopOk, stats, statsOk);

            int ledgerIndex = -1;
            for (int i = 0; i < _obsSharedTickets.Length; i++)
            {
                if (_obsSharedTickets[i].ticket == 0UL && !_obsSharedOwned[i] && !_obsSharedCommitted[i])
                { ledgerIndex = i; break; }
            }
            if (ledgerIndex < 0)
            {
                // All three managed ledger entries still hold an owned/committed packet. On the plain
                // 1 ms timer against ~11 ms XR cadence this is NORMAL backpressure, not a fault: skip
                // this tick. NEVER RecordHostedCaptureFailure (that would permanently close the owner),
                // never release/overwrite a retained ticket, never fall back, never retry this tick.
                _obsSharedBackpressureSkips++;
                return false;
            }

            ulong reservedFrameSerial = ++_frameSerial;   // reserve a fresh nonzero serial (shared counter)

            // ---- freeze the 192-byte PREPARE identity BY VALUE (captureEndQpc MUST be 0; no SDK object) ----
            long captureBeginQpc = Stopwatch.GetTimestamp();   // AFTER the pose end stamp
            if (captureBeginQpc < poseEndQpc)
            {
                RecordHostedCaptureFailure(originalGeneration, "obs_shared_capture_begin_before_pose_end");
                return false;
            }
            var prepIdentity = new VRSIMPlugin.SharedGpuCaptureIdentityV1
            {
                structBytes = ObsSharedIdentityBytesV1,
                version = ObsSharedWireVersionV1,
                frameSerial = reservedFrameSerial,
                poseSerial = poseSerial,
                epoch = nativeEpoch,
                generation = (uint)originalGeneration,
                reserved = 0u,
                poseSampleBeginQpc = poseBeginQpc,
                poseSampleEndQpc = poseEndQpc,
                captureBeginQpc = captureBeginQpc,
                captureEndQpc = 0L,
                cameraYawDeg = _lastYawDelta,
                leftPlan = leftPlan,
                rightPlan = rightPlan
            };

            // shouldStop allocated BEFORE entering the owner, bound to the ORIGINAL refs/generation; uses
            // managed/numeric gates ONLY — no unowned SDK snapshot inside the owner lock.
            Func<bool> shouldStop = () =>
                !_running ||
                _generation != originalGeneration ||
                _hostedStartGeneration != originalGeneration ||
                !ReferenceEquals(_hostedOwner, owner);

            // Synchronous PREPARE callback: calls ONLY PrepareSharedGpu, writes the full returned ticket +
            // original identity to the preselected ledger BEFORE classification, returns a positive ticket
            // ONLY for an exact Completed grant. Never COMMIT here, never stores the borrowed surface
            // pointers, never reads back pixels.
            Func<ulong, ulong, ulong> prepareCallback = (leftSurface, rightSurface) =>
            {
                VRSIMPlugin.SharedGpuTicketV1 granted;
                int prc = _host.PrepareSharedGpu(leftSurface, rightSurface, ref prepIdentity, out granted);

                _obsSharedTickets[ledgerIndex] = granted;
                _obsSharedIdentities[ledgerIndex] = prepIdentity;
                _obsSharedOwned[ledgerIndex] = granted.ticket != 0UL;   // retain any nonzero grant for exact DISCARD

                if (prc == (int)VRSIMPlugin.SharedGpuResultV1.Pending && granted.ticket == 0UL)
                    throw new OperationCanceledException("OBS_SHARED_PREPARE_PENDING");   // backpressure, non-sticky

                if (prc == (int)VRSIMPlugin.SharedGpuResultV1.Completed &&
                    granted.version == ObsSharedWireVersionV1 &&
                    granted.structBytes == ObsSharedTicketBytesV1 &&
                    granted.reservedPadding == 0u &&
                    granted.slotIndex < ObsSharedSlotCountV1 &&
                    granted.width == ObsSharedTicketEdgeV1 &&
                    granted.height == ObsSharedTicketEdgeV1 &&
                    granted.format88 == ObsSharedTicketFormat88V1 &&
                    granted.generation == (uint)originalGeneration &&
                    granted.epoch == nativeEpoch &&
                    granted.ticket != 0UL)
                {
                    _obsSharedPrepared++;
                    return granted.ticket;
                }

                throw new InvalidOperationException(
                    "OBS_SHARED_PREPARE_FAULT rc=" + prc.ToString(CultureInfo.InvariantCulture) +
                    " ticket=" + granted.ticket.ToString(CultureInfo.InvariantCulture));
            };

            // Holds the ORIGINAL owner/native body exception object (full stack + the owner's Unlock
            // AggregateException with its inner exceptions intact) so the finally can aggregate it with
            // any DISCARD fault WITHOUT overwriting or losing it. A clean return / normal cancel leaves null.
            Exception bodyFault = null;
            try
            {
                NC04.NC01GpuPreparedPairV1 prepared = owner.CapturePreparedSharedRawPair(
                    leftRaw, rightRaw, sourceFovLeft, sourceFovRight, 1.0, true,
                    originalGeneration, shouldStop, prepareCallback);

                // Owner returned normally → reverse double-Unlock, main/quality/projection postguards and
                // the atomic original-generation commit already happened (owner215-318). Read RETURNED
                // managed values ONLY — no extra SDK read, no owner rerun.
                var ledgerTicket = _obsSharedTickets[ledgerIndex];
                if (prepared == null || ledgerTicket.ticket == 0UL ||
                    prepared.PreparedTicket != ledgerTicket.ticket)
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                        RecordHostedCaptureFailure(originalGeneration, "obs_shared_owner_ticket_mismatch");
                    return false;
                }
                if (prepared.Generation != originalGeneration ||
                    prepared.Width != ObsSharedTicketEdgeV1 || prepared.Height != ObsSharedTicketEdgeV1 ||
                    !prepared.MainPreserved ||
                    !prepared.LeftUnlocked || !prepared.RightUnlocked ||
                    prepared.LeftRenderRc < 0 || prepared.RightRenderRc < 0)
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                        RecordHostedCaptureFailure(originalGeneration,
                            "obs_shared_owner_postguard_inconsistent gen=" + prepared.Generation +
                            " wh=" + prepared.Width + "x" + prepared.Height +
                            " mainPreserved=" + (prepared.MainPreserved ? 1 : 0) +
                            " LUnlock=" + (prepared.LeftUnlocked ? 1 : 0) +
                            " RUnlock=" + (prepared.RightUnlocked ? 1 : 0) +
                            " Lrc=" + prepared.LeftRenderRc + " Rrc=" + prepared.RightRenderRc);
                    return false;
                }

                // Stamp the real captureEnd EXACTLY ONCE; COMMIT identity = value copy of the stored PREPARE
                // identity with ONLY captureEndQpc changed (plan/yaw IEEE bit patterns preserved).
                long captureEndQpc = Stopwatch.GetTimestamp();
                var commitIdentity = _obsSharedIdentities[ledgerIndex];
                commitIdentity.captureEndQpc = captureEndQpc;
                if (!(commitIdentity.poseSampleBeginQpc > 0L &&
                      commitIdentity.poseSampleBeginQpc <= commitIdentity.poseSampleEndQpc &&
                      commitIdentity.poseSampleEndQpc <= commitIdentity.captureBeginQpc &&
                      commitIdentity.captureBeginQpc < commitIdentity.captureEndQpc))
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                        RecordHostedCaptureFailure(originalGeneration, "obs_shared_chronological_qpc_invalid");
                    return false;
                }

                // Re-check ORIGINAL running/gen/owner/admission/EntryLocked + native-open + Stats120 open
                // admission with the EXACT captured epoch. A stop/epoch change REJECTS this pair, never rebinds.
                var gst = _host.ReadGpuPipelineStats();
                if (!_running || _generation != originalGeneration ||
                    _hostedStartGeneration != originalGeneration || !ReferenceEquals(_hostedOwner, owner) ||
                    owner.BoundGeneration != originalGeneration || !owner.AdmissionOpen ||
                    _cal == null || _cal.State != EntryCalState.EntryLocked ||
                    _host.NativeStateForMonitor() != 6 ||
                    !gst.HasValue || gst.Value.reserved != 1u || gst.Value.sessionEpoch != nativeEpoch)
                {
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                        RecordHostedCaptureFailure(originalGeneration,
                            "obs_shared_commit_recheck_failed nativeState=" + _host.NativeStateForMonitor() +
                            " statsEpoch=" + (gst.HasValue ? gst.Value.sessionEpoch : 0UL) +
                            " statsReserved=" + (gst.HasValue ? gst.Value.reserved : 0u) +
                            " ticketEpoch=" + nativeEpoch);
                    return false;
                }

                // COMMIT the EXACT original ticket + completion identity OUTSIDE the owner callback/locks.
                var commitTicket = _obsSharedTickets[ledgerIndex];
                int crc = _host.CommitSharedGpu(ref commitTicket, ref commitIdentity);
                if (crc == (int)VRSIMPlugin.SharedGpuResultV1.Completed)
                {
                    // Native Completed transfers PREPARED ownership to the native worker/ring. The FULL
                    // ticket + identity are RETAINED (committed=true, owned=false) so the Managed3B stop
                    // card can still exact-Discard an unread COMMITTED packet — never dropped here.
                    _obsSharedCommitAccepted++;
                    _obsSharedOwned[ledgerIndex] = false;
                    _obsSharedCommitted[ledgerIndex] = true;
                    return true;
                }

                // Pending/error/quarantine COMMIT: retain the original ticket owned for exact cleanup —
                // no COMMIT retry, no newer identity, no rewritten end timestamp. finally exact-Discards it.
                _obsSharedRejected++;
                if (_running && _generation == originalGeneration &&
                    _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                    RecordHostedCaptureFailure(originalGeneration,
                        "obs_shared_commit_not_completed rc=" + crc.ToString(CultureInfo.InvariantCulture));
                return false;
            }
            catch (OperationCanceledException)
            {
                // Backpressure / explicit Stop / owner cancel — a NORMAL skipped tick, NOT a fault, NOT a
                // CPU fallback, NOT a retry. Any nonzero grant stays in the ledger for exact-DISCARD below.
                return false;
            }
            catch (Exception ex)
            {
                // Genuine owner/native fault. Keep the ORIGINAL exception OBJECT (full stack, including
                // the owner's reverse double-Unlock AggregateException with its inner exceptions intact)
                // — never reduce it to Message. Convergence + Log happen in the finally so any DISCARD
                // fault is COMBINED WITHOUT overwriting this body fault.
                bodyFault = ex;
                _obsSharedRejected++;
                return false;
            }
            finally
            {
                // EXACT DISCARD of a still-locally-owned packet FIRST (cancel / postguard exception /
                // invalid result / stale epoch / commit refusal / any later fault). Returns the real
                // discard fault or null when clean; a failed discard RETAINS the packet (no ticket-field
                // change, no CPU fallback, no owner rerun) — only a native rc==0 clears the entry.
                Exception discardFault = null;
                if (ledgerIndex >= 0 && _obsSharedOwned[ledgerIndex])
                    discardFault = TryDiscardObsSharedPrepared(ledgerIndex);

                // Combine the body fault and the discard fault WITHOUT overwriting either original
                // exception/stack. A pure OperationCanceledException cancel/backpressure leaves bodyFault
                // null (NOT a fault), but a discard failure is always a real fault and is retained.
                Exception combined = Combine(bodyFault, discardFault);
                if (combined != null)
                {
                    // Always cache + Log the FULL combined evidence (stack + inner exceptions) for
                    // read-only QA — even after an earlier body fault already closed _running.
                    _obsSharedLastFault = combined;
                    _host.Log("OBS_SHARED_FAULT_EVIDENCE " + combined.ToString());

                    // Converge through RecordHostedCaptureFailure ONLY while the ORIGINAL generation/refs/
                    // running are still valid — never overwrite a previously recorded original exception,
                    // never restart teardown, never re-run the owner.
                    if (_running && _generation == originalGeneration &&
                        _hostedStartGeneration == originalGeneration && ReferenceEquals(_hostedOwner, owner))
                        RecordHostedCaptureFailure(originalGeneration,
                            "obs_shared_fault " + combined.GetType().Name + ": " + combined.Message);
                }
            }
        }

        private void TickCore()
        {
            if (!_running || !_entryCaptured) return;
            // NC02-XR-P5: freeze the ORIGINAL generation/owner for this whole tick — a Stop or a
            // rebind mid-tick must never re-bind staging work to the current _generation.
            int nc02OriginalGeneration = _hostedStartGeneration;
            var nc02OriginalOwner = _hostedOwner;
            // ---- hosted-d3d9 readiness prefix (reviewed03): a PURE managed gate BEFORE the GPU
            // epoch read, tracking, calibration, preview/capture/worker/native submit. A Stop or
            // generation invalidation ends the tick silently — no cached SDK failure, no second
            // teardown notification. Only an owned-stage Ready inside the SAME original generation
            // with open admission and no sticky failure may fall through; a late Ready can never
            // resurrect a closed generation. Category A (TryGetReady=false, admission open, no
            // failure — pendingReason allowed to be non-null) stays await_hosted_ready and is
            // NEVER fatal; category B (owner FailureReason set, or the admission of a still-valid
            // generation unexpectedly closed, readiness exception, 15 s timeout) converges through
            // RecordHostedCaptureFailure. The 15 s QPC timeout is evaluated only when this timer
            // gets an execution slot — not a hard deadline against a blocking SDK call.
            if (_hostedBackendSelected)
            {
                int hostedGen = _hostedStartGeneration;
                var hostedOwner = _hostedOwner;
                if (hostedGen < 0 || hostedOwner == null ||
                    _generation != hostedGen || hostedOwner.BoundGeneration != hostedGen ||
                    _hostedFailureGeneration == hostedGen)
                    return;   // stopped / generation invalidated / failure already converged

                string ownerFailure = hostedOwner.FailureReason;
                if (!string.IsNullOrEmpty(ownerFailure))
                {
                    RecordHostedCaptureFailure(hostedGen, "hosted_owner_failure " + ownerFailure);
                    return;
                }
                if (!hostedOwner.AdmissionOpen)
                {
                    RecordHostedCaptureFailure(hostedGen,
                        "hosted_admission_closed_unexpected generation=" + hostedGen);
                    return;
                }

                if (!IsCaptureBackendReady)
                {
                    bool ready;
                    string pendingReason;
                    try
                    {
                        ready = hostedOwner.TryGetReady(hostedGen, out pendingReason);
                    }
                    catch (Exception rex)
                    {
                        // never let a readiness exception escape into the DispatcherTimer
                        if (_running && _generation == hostedGen && hostedOwner.BoundGeneration == hostedGen)
                            RecordHostedCaptureFailure(hostedGen,
                                "hosted_readiness_exception " + rex.GetType().Name + ": " + rex.Message);
                        return;
                    }
                    if (!_running || _generation != hostedGen || hostedOwner.BoundGeneration != hostedGen)
                        return;   // explicit Stop / invalidation raced the call — no cache, no record
                    if (!ready)
                    {
                        ownerFailure = hostedOwner.FailureReason;
                        if (!string.IsNullOrEmpty(ownerFailure))
                        {
                            RecordHostedCaptureFailure(hostedGen, "hosted_owner_failure " + ownerFailure);
                            return;
                        }
                        if (!hostedOwner.AdmissionOpen)
                        {
                            RecordHostedCaptureFailure(hostedGen,
                                "hosted_admission_closed_unexpected generation=" + hostedGen);
                            return;
                        }
                        // category A — NORMAL pending (view not loaded / own stage unconfirmed are
                        // valid pendingReason values; false or a non-null reason alone is NOT fatal).
                        if (_hostedAwaitGeneration != hostedGen)
                        {
                            _hostedAwaitGeneration = hostedGen;
                            _hostedAwaitSinceQpc = Stopwatch.GetTimestamp();
                        }
                        double awaitedMs = (Stopwatch.GetTimestamp() - _hostedAwaitSinceQpc) *
                            1000.0 / Stopwatch.Frequency;
                        if (awaitedMs >= HostedReadyTimeoutMs)
                            RecordHostedCaptureFailure(hostedGen,
                                "hosted_readiness_timeout_15s awaitedMs=" +
                                awaitedMs.ToString("0", CultureInfo.InvariantCulture) +
                                " lastPendingReason=" + (string.IsNullOrEmpty(pendingReason) ? "none" : pendingReason));
                        return;   // await_hosted_ready — no preview/capture/worker/submit this tick
                    }
                    // Ready=true must ALSO hold the open admission, the same original generation
                    // and no sticky failure — otherwise it is category B, not a pass.
                    if (!hostedOwner.AdmissionOpen || !string.IsNullOrEmpty(hostedOwner.FailureReason) ||
                        hostedOwner.BoundGeneration != hostedGen)
                    {
                        ownerFailure = hostedOwner.FailureReason;
                        RecordHostedCaptureFailure(hostedGen, string.IsNullOrEmpty(ownerFailure)
                            ? "hosted_ready_with_closed_or_rebound_generation generation=" + hostedGen
                            : "hosted_owner_failure " + ownerFailure);
                        return;
                    }
                }
                // owned Ready inside the open, same-generation, failure-free admission → fall
                // through to tracking/calibration/capture (hosted capture backend: later cards).
            }
            // NC02-XR-P5: staging collect gate — BEFORE the GPU epoch read and any fresh XR pose
            // read, at most ONE owner poll per tick. A completed staging pair is submitted with
            // its OWN frozen ticket identity; the current epoch may never overwrite an old
            // ticket, and while anything is still pending the tick captures no second pair.
            if (_nc02StagingRequested && _cal != null && _cal.State == EntryCalState.EntryLocked)
            {
                try
                {
                    NC02XrQA.NC02SourcePairResultV1 completed;
                    Nc02XrCaptureMetaRow stagedRow;
                    string collectReason;
                    bool collected = TryCollectNc02StagingPair(nc02OriginalOwner, nc02OriginalGeneration,
                        out completed, out stagedRow, out collectReason);
                    if (!_running || _generation != nc02OriginalGeneration ||
                        nc02OriginalOwner == null ||
                        nc02OriginalOwner.BoundGeneration != nc02OriginalGeneration ||
                        !nc02OriginalOwner.AdmissionOpen)
                        return;   // owner/gen/running lost or admission closed — end this tick
                    if (collected)
                    {
                        _renderSuccess++;
                        _captureSerial++;
                        // the FULL synchronous capture cost is UNKNOWN on this path — never fill
                        // it with source-queue latency
                        _renderMs = double.NaN;
                        _lastStereoPairMs = double.NaN;
                        var stgst = _host.ReadGpuPipelineStats();
                        if (!stgst.HasValue || stgst.Value.reserved != 1u ||
                            stgst.Value.sessionEpoch != stagedRow.Identity.NativeXrEpoch)
                        {
                            RecordHostedCaptureFailure(nc02OriginalGeneration,
                                "nc02_collect_epoch_mismatch ticketEpoch=" + stagedRow.Identity.NativeXrEpoch +
                                " currentEpoch=" + (stgst.HasValue ? stgst.Value.sessionEpoch : 0UL) +
                                " frameSerial=" + stagedRow.Identity.FrameSerial);
                            return;   // never push an old ticket onto a mismatched epoch
                        }
                        // ONE native raw enqueue, strictly from the frozen ticket — never a fresh
                        // pose, BuildEyeProjectionPlan, _poseSerialUsed or _lastYawDelta
                        var submitRc = _host.SubmitRawStereo(
                            completed.LeftBgra, completed.RightBgra,
                            stagedRow.Width, stagedRow.Height, stagedRow.Stride,
                            stagedRow.Identity.FrameSerial, stagedRow.Identity.PoseSerial,
                            stagedRow.Identity.NativeXrEpoch,
                            stagedRow.LeftPlan, stagedRow.RightPlan, stagedRow.YawDeg);
                        if (submitRc == 0)
                        {
                            _framesSubmitted++;
                            _liveSubmitted++;
                            _nc02NativeEnqueueAccepted++;
                            long acceptedQpc = Stopwatch.GetTimestamp();
                            // conservative age from the SAMPLE START of the frozen ticket
                            double poseToEnqueueMs = (acceptedQpc - stagedRow.Identity.PoseSampleStartQpc) *
                                1000.0 / Stopwatch.Frequency;
                            if (acceptedQpc > stagedRow.Identity.PoseSampleStartQpc &&
                                acceptedQpc >= stagedRow.Identity.PoseSampleEndQpc &&
                                !double.IsNaN(poseToEnqueueMs) && !double.IsInfinity(poseToEnqueueMs) &&
                                poseToEnqueueMs >= 0.0)
                                _nc02LastPoseToEnqueueMs = poseToEnqueueMs;
                            // double.NaN = synchronous capture cost unknown; the helper still
                            // honestly records trueCaptureBegin -> native accepted
                            NoteGpuEnqueueSuccess(stagedRow.Identity.CaptureBeginQpc, acceptedQpc, double.NaN);
                        }
                        else
                        {
                            _submitFailures++;
                            _nc02Dropped++;
                            if (_gpuSubmitLogCount < 10)
                            {
                                _gpuSubmitLogCount++;
                                _host.Log("VC_NC02_STAGING_NATIVE_ENQUEUE_FAIL rc=" + submitRc +
                                    " frameSerial=" + stagedRow.Identity.FrameSerial +
                                    " logged=" + _gpuSubmitLogCount + "/10 totalDrops=" + _nc02Dropped);
                            }
                            // no display claim, no fallback, never re-push the same result
                        }
                    }
                    // regardless of the collect result: while ANY managed ref is live, this
                    // tick reads no fresh XR pose and captures no second pair
                    if (_nc02CaptureRows.Count != 0 ||
                        _nc02MapPairIds[0] != 0UL || _nc02MapPairIds[1] != 0UL ||
                        _nc02MapFrameSerials[0] != 0UL || _nc02MapFrameSerials[1] != 0UL ||
                        nc02OriginalOwner.StagingPendingCount != 0)
                        return;
                }
                catch (Exception cex)
                {
                    if (_running && _generation == nc02OriginalGeneration &&
                        nc02OriginalOwner != null &&
                        nc02OriginalOwner.BoundGeneration == nc02OriginalGeneration)
                        RecordHostedCaptureFailure(nc02OriginalGeneration,
                            "nc02_collect_submit_exception " + cex.GetType().Name + ": " + cex.Message);
                    return;   // never let an exception escape into the DispatcherTimer
                }
            }
            // H2-GPU: periodic >=10 s raw-enqueue flush (GPU's own telemetry window), then
            // record the native session epoch BEFORE the tracking read and the stereo
            // capture. The epoch is bound to THIS tick's capture; it is NEVER re-read or
            // re-bound to a fresher epoch after CaptureStereoPair. Requirements from the
            // 120-byte V1 wire: valid read (host enforces 120/v1), reserved bit0 open with
            // all other reserved bits zero, and sessionEpoch > 0 — otherwise skip the tick
            // with a bounded message (no silent capture without an honest epoch binding).
            ulong capturedNativeEpoch = 0UL;
            if (_gpuPipeline)
            {
                WriteGpuWindow(Stopwatch.GetTimestamp(), false);
                var gst = _host.ReadGpuPipelineStats();
                if (!gst.HasValue || gst.Value.reserved != 1u || gst.Value.sessionEpoch == 0UL)
                {
                    _gpuEpochSkips++;
                    if (_gpuEpochSkipLogCount < 10)
                    {
                        _gpuEpochSkipLogCount++;
                        _host.Log("VC_GPU_EPOCH_SKIP reason=" +
                            (!gst.HasValue ? "stats_wire_unavailable"
                                : (gst.Value.reserved != 1u ? "reserved_admission_not_open_or_dirty_bits"
                                                            : "session_epoch_zero")) +
                            " logged=" + _gpuEpochSkipLogCount + "/10 totalSkips=" + _gpuEpochSkips);
                    }
                    return;   // LAST_KNOWN_GOOD — no honest epoch binding this tick
                }
                capturedNativeEpoch = gst.Value.sessionEpoch;
            }
            // ASYNC PIPELINE: poll AT MOST ONE completed worker result BEFORE reading a fresh
            // pose and submit it with the RESULT's serials. The rest of this tick is capture +
            // enqueue — no CPU resample runs here. The old RecordPacing/performance-10s.json
            // tick-interval telemetry was mislabeled content and is REMOVED; async completion
            // telemetry goes to async-pipeline-10s.json (writer: next card).
            if (!_gpuPipeline) PollCompletedFrame();
            // Periodic ≥10 s flush BEFORE the tracking early-returns below; the _asyncFinalWritten
            // guard inside WriteAsyncWindow means nothing is ever emitted after the stop flush.
            if (!_gpuPipeline) WriteAsyncWindow(Stopwatch.GetTimestamp(), false);
            try
            {
                // ---- PHASE 12: LATEST_POSE_WINS — freshest runtime eye poses (stats→views→stats
                // stable-serial bracket via the host helper; the serial is the job/metadata binding) ----
                VRSIMPlugin.VRSIMEyeView l, r;
                ulong poseSerial;
                // NC02-XR-P5: measure the REAL stable-bracket window around the actual pose read
                // (only when staging is requested). These are the pose-sample QPCs — NOT the
                // result-capture window; SourceLeaseEpoch stays 0 and no static source-epoch/
                // pose-0 API is ever called on this path.
                long nc02PoseSampleStartQpc = (_nc02StagingRequested || _obsSharedRequested) ? Stopwatch.GetTimestamp() : 0L;
                bool viewsOk = _host.TryGetViewsWithSerial(out l, out r, out poseSerial);
                long nc02PoseSampleEndQpc = (_nc02StagingRequested || _obsSharedRequested) ? Stopwatch.GetTimestamp() : 0L;
                if (!viewsOk || !IsFinite(l) || !IsFinite(r))
                {
                    _invalidTrackingFrames++;
                    return; // LAST_KNOWN_GOOD = keep last submitted frame on screen (PHASE 20)
                }
                _poseSerialUsed = poseSerial;
                // RAW eye quats must pass ValidQ BEFORE normalization; poseSerial>0 and the
                // native frame-state trackingValid are required (no fake-valid data, no reliance
                // on the sensitive presence sensor). Invalid → LAST_KNOWN_GOOD (PHASE 20).
                if (!ValidQ(l.quatXyzw) || !ValidQ(r.quatXyzw) || _poseSerialUsed == 0 || !_host.IsTrackingValid())
                { _invalidTrackingFrames++; return; }

                double[] headL = { l.posMm[0], l.posMm[1], l.posMm[2] };
                double[] headR = { r.posMm[0], r.posMm[1], r.posMm[2] };
                double[] center = { (headL[0] + headR[0]) / 2, (headL[1] + headR[1]) / 2, (headL[2] + headR[2]) / 2 };
                double[] qHead = NormalizeQ(l.quatXyzw);

                if (_cal == null)
                {
                    _cal = new EntryCalibration { FocusedGate = _host.IsSessionFocused() };   // honest gate, refreshed per tick
                    var st0 = _host.ReadStereoStats();
                    bool avail = st0.HasValue && st0.Value.userPresenceAvailable == 1;
                    _cal.SetPresenceCapability(avail);
                    _presenceSignal = avail ? "OPENXR_USER_PRESENCE" : "FALLBACK_HEURISTIC";
                    _host.Log("ENTRY_CALIBRATION presenceSignal=" + _presenceSignal +
                        " presenceAvail=" + avail +
                        " supported=" + (st0.HasValue ? st0.Value.userPresenceSupported : -1));
                    _host.MsgOnce("[VRSIM] Waiting for headset…", "waiting");
                }
                else _cal.FocusedGate = _host.IsSessionFocused();   // no fake FOCUSED (review item 4)

                if (_cal.State != EntryCalState.EntryLocked)
                {
                    // 02C-CALIBRATION PHASE 2-7: presence + stability window decide when to lock.
                    // Before lock: ALWAYS the untouched entry desktop view (PHASE 6) — desk poses,
                    // pickups and wearing motions can never leak into the VC camera.
                    var stNow = _host.ReadStereoStats();
                    int presence = stNow.HasValue ? (stNow.Value.isUserPresent >= 0 ? stNow.Value.isUserPresent : -1) : -1;
                    bool validForCalib = IsFinite(l) && IsFinite(r) && ValidQ(l.quatXyzw) && ValidQ(r.quatXyzw)
                        && _host.IsTrackingValid() && _poseSerialUsed > 0;
                    var prevState = _cal.State;
                    bool locked = _cal.Feed(Environment.TickCount, center, qHead, presence, validForCalib);
                    if (prevState == EntryCalState.WaitingForUser && _cal.State == EntryCalState.CalibratingEntry)
                        _host.MsgOnce("[VRSIM] Calibrating view…", "calibrating");
                    if (locked)
                    {
                        _head0 = center; _qHead0 = qHead;
                        _originCaptured = true;
                        _preCalibrationFrame = null;   // live per-eye capture takes over — static preview dropped
                        ClearHostedPreviewCaches();    // hosted static raw preview dropped on the SAME lock (isolated caches, one source of truth)
                        _host.WriteVrEntryOrigin(center, qHead);
                        _host.MsgOnce("[VRSIM] VR Ready — 入口视角已锁定", "ready");
                        _host.Log("ENTRY_LOCKED head=(" +
                            center[0].ToString("0.0", CultureInfo.InvariantCulture) + "," +
                            center[1].ToString("0.0", CultureInfo.InvariantCulture) + "," +
                            center[2].ToString("0.0", CultureInfo.InvariantCulture) + ") stableMs=" +
                            _cal.StableDurationMs.ToString("0.0", CultureInfo.InvariantCulture) +
                            " jitterDeg=" + _cal.OrientationJitterDeg.ToString("0.00", CultureInfo.InvariantCulture) +
                            " jitterMm=" + _cal.PositionJitterMm.ToString("0.0", CultureInfo.InvariantCulture));
                    }
                    else
                    {
                        // OBS-SHARED pre-calibration: WAIT only — never SubmitEntryFrame / the hosted
                        // static CPU preview / a pose0 static frame merely to fill the headset.
                        if (_obsSharedRequested) return;
                        SubmitEntryFrame();
                        return;
                    }
                }

                // ---- PHASE 4/6 (02C-FIX): entry-LOCAL relative delta — validated offline
                // (entry-invariant + direction probes + mapping immunity all PASS).
                // R_D = R_h0^T·R_h ; p_D = R_h0^T·(center−head0)  [entry-head-local]
                // R_vc = R_E·R_D  ; P_vc = P_E + R_E·p_D
                // eye: R_cam = R_vc·(R_h^T·R_eye) ; P_cam = P_vc + R_vc·(R_h^T·(P_eye−center))
                double[] R_h = QToMatrix(qHead);
                double[] R_h0 = QToMatrix(_qHead0);
                double[] R_D = Mul3(Trans3(R_h0), R_h);
                double[] p_D = Apply3(Trans3(R_h0), Sub(center, _head0));
                double[] R_vc = Mul3(_R_E, R_D);
                double[] P_vc = Add(_P_E, Apply3(_R_E, p_D));

                double[] R_relL = Mul3(Trans3(R_h), QToMatrix(NormalizeQ(l.quatXyzw)));
                double[] R_relR = Mul3(Trans3(R_h), QToMatrix(NormalizeQ(r.quatXyzw)));
                double[] p_relL = Apply3(Trans3(R_h), Sub(headL, center));
                double[] p_relR = Apply3(Trans3(R_h), Sub(headR, center));
                double[] R_Lcam = Mul3(R_vc, R_relL);
                double[] R_Rcam = Mul3(R_vc, R_relR);
                double[] P_Lcam = Add(P_vc, Apply3(R_vc, p_relL));
                double[] P_Rcam = Add(P_vc, Apply3(R_vc, p_relR));

                if (!AllFinite(R_Lcam) || !AllFinite(P_Lcam) || !AllFinite(R_Rcam) || !AllFinite(P_Rcam))
                { _invalidTrackingFrames++; return; }

                Matrix mL = RotPosToMat(R_Lcam, P_Lcam);
                Matrix mR = RotPosToMat(R_Rcam, P_Rcam);

                // camera-change evidence (PHASE 14/15)
                if (_haveLastCam)
                {
                    double dd = Math.Sqrt(Sq(P_Lcam[0] - _lastCamL[0]) + Sq(P_Lcam[1] - _lastCamL[1]) + Sq(P_Lcam[2] - _lastCamL[2]));
                    if (dd > 0.5) _cameraChangeCount++;
                }
                _lastCamL = P_Lcam; _haveLastCam = true;
                LogHeadDeltas(qHead);

                // ---- 02D: per-eye asymmetric XR fov → plan BEFORE capture. The symmetric source
                // frustum (ty/tx max rule) covers the FULL real XR box, then CPU resampling aligns
                // the image rays with the headset projection. Invalid per-eye fov → skip the frame
                // with evidence, never submit a wrong picture.
                var pl = BuildEyeProjectionPlan(l.fovLeft, l.fovRight, l.fovUp, l.fovDown);
                var pr = BuildEyeProjectionPlan(r.fovLeft, r.fovRight, r.fovUp, r.fovDown);
                if (!pl.Valid || !pr.Valid)
                {
                    _fovInvalidSkips++;
                    string reason = "L=" + (pl.Valid ? "ok" : pl.RejectReason) + " R=" + (pr.Valid ? "ok" : pr.RejectReason);
                    if (reason != _lastFovSkipReason)
                    {
                        _lastFovSkipReason = reason;
                        _host.Log("EYE_FOV_INVALID skip_frame " + reason + " totalSkips=" + _fovInvalidSkips);
                    }
                    return;   // LAST_KNOWN_GOOD; no SDK restore needed here — the owner already restored after the last capture
                }
                WriteProjectionEvidenceOnce(pl, pr);

                // OBS-SHARED (Card3A): after the real calibration, both eye plans and the evidence hook
                // are complete, and BEFORE NC02 staging / the synchronous byte capture, run ONE typed
                // PREPARE→managed-return→COMMIT owner transaction with the ORIGINAL frozen owner/
                // generation, the raw world-to-camera mL/mR (never inverted), the wire plans from the
                // existing MakeRawProjectionPlan, actual per-eye SourceFovYDeg, the real poseSerial/
                // native epoch and the genuine pose QPC bracket — then end the tick UNCONDITIONALLY.
                // Shared may NEVER fall into NC02 collect/submit, CaptureStereoPair, ForceAlpha,
                // SubmitRawStereo, StoreCaptureMeta, worker byte enqueue, PNG or CPU resampling.
                if (_obsSharedRequested && _cal != null && _cal.State == EntryCalState.EntryLocked)
                {
                    var swpl = VRSIMPlugin.MakeRawProjectionPlan(pl);
                    var swpr = VRSIMPlugin.MakeRawProjectionPlan(pr);
                    TrySubmitObsSharedCapture(nc02OriginalOwner, nc02OriginalGeneration,
                        mL, mR, swpl, swpr,
                        pl.SourceFovYDeg, pr.SourceFovYDeg,
                        poseSerial, capturedNativeEpoch,
                        nc02PoseSampleStartQpc, nc02PoseSampleEndQpc);
                    return;
                }

                // NC02-XR-P5 (staging ON): hand THIS tick's real pose/plan/calibration numbers to
                // the bounded owner source-staging submit, then end the tick unconditionally.
                // ON never falls into the old synchronous CaptureStereoPair below; the OFF path
                // is preserved verbatim. Source matrices/FOV are the SAME fresh XR pose — never
                // a desktop main/newpose substitution.
                if (_nc02StagingRequested && _cal != null && _cal.State == EntryCalState.EntryLocked)
                {
                    var wpl = VRSIMPlugin.MakeRawProjectionPlan(pl);
                    var wpr = VRSIMPlugin.MakeRawProjectionPlan(pr);
                    TrySubmitNc02StagingCapture(nc02OriginalOwner, nc02OriginalGeneration,
                        mL, mR, l, r, wpl, wpr,
                        pl.SourceFovYDeg, pr.SourceFovYDeg,
                        poseSerial, capturedNativeEpoch,
                        nc02PoseSampleStartQpc, nc02PoseSampleEndQpc);
                    return;
                }

                // ---- LIVE CAPTURE (02F worker integration): ONE guarded owner.CaptureStereo
                // runs on this tick (SDK-verified: same L/R pixels and disparity as two sequential
                // Capture calls, desktop restoration verified; QA is finite-sample only — NO FPS
                // claim). The XR resample happens on the worker; the completion submit, evidence
                // and telemetry bind to the RESULT's serials (next card) — never re-bound to a
                // fresher pose here. capturePairMs is a real Stopwatch duration over the paired
                // call AND the two ForceAlpha passes — never an inferred split.
                _renderAttempts++;
                long captureStartQpc = Stopwatch.GetTimestamp();
                var swPair = Stopwatch.StartNew();
                // Snapshot the generation and the ORIGINAL capture pose serial immediately
                // BEFORE the paired capture: a StopAndRestore running during SDK capture's UI
                // pumping must never leave this tick touching the owner's buffers or submitting.
                int captureGeneration = _generation;
                ulong capturedPoseSerial = _poseSerialUsed;
                HostedPairTiming? hostedPairTiming;   // populated only by a validated hosted pair; never used unless the submit is accepted
                byte[][] pair = CaptureStereoPair(mL, pl.SourceFovYDeg, mR, pr.SourceFovYDeg, out hostedPairTiming);
                if (pair == null || pair.Length != 2 || pair[0] == null || pair[1] == null ||
                    pair[0].Length != CaptureProfile.PayloadBytes ||
                    pair[1].Length != CaptureProfile.PayloadBytes)
                    throw new InvalidOperationException("producer_stereo_reject reason=malformed_pair");
                byte[] rawL = pair[0];
                byte[] rawR = pair[1];
                // Post-return safety (CPU AND GPU): payload validated, then still-running and
                // unchanged generation — otherwise drop the pair safely without touching it.
                if (!_running || captureGeneration != _generation ||
                    (_hostedBackendSelected && !CaptureAdmissionOpen))
                {
                    if (_postCaptureDropLogCount < 10)
                    {
                        _postCaptureDropLogCount++;
                        _host.Log("VC_POST_CAPTURE_DROP running=" + (_running ? "true" : "false") +
                            " captureGeneration=" + captureGeneration + " generation=" + _generation +
                            " logged=" + _postCaptureDropLogCount + "/10");
                    }
                    return;
                }
                double capturePairMs;
                if (_gpuPipeline)
                {
                    // GPU: NO ForceAlpha and no CPU resample (the projection shader forces
                    // alpha=1); capturePairMs covers the ACTUAL owner pair only — never a
                    // fake GPU duration.
                    capturePairMs = swPair.Elapsed.TotalMilliseconds;
                }
                else
                {
                    ForceAlpha(rawL);
                    ForceAlpha(rawR);
                    capturePairMs = swPair.Elapsed.TotalMilliseconds;   // unchanged CPU timing window
                }
                double capL = double.NaN, capR = double.NaN;   // paired mode: per-eye split is UNKNOWN
                // The paired capture returned → the render attempt succeeded; any exception exits
                // to the catch below and must NOT increment _renderSuccess.
                _renderMs = (Stopwatch.GetTimestamp() - captureStartQpc) * 1000.0 / Stopwatch.Frequency;
                _renderSuccess++;
                _lastStereoPairMs = capturePairMs;

                _captureSerial++;                       // capture counter only — not the wire serial
                ulong wireSerial = ++_frameSerial;      // wire serial shared with the static calibration preview
                if (_gpuPipeline)
                {
                    // GPU branch: the symmetric-source raw pair goes DIRECTLY to the native
                    // runtime layer — no CPU resample job, no StoreCaptureMeta, and NO
                    // producer PNG / motion evidence claiming headset pixels: raw captures are
                    // NOT projected eyes and must never be saved as projected-stereo evidence.
                    // The projected ACTUAL pixels for technical QA come from the worker-owned
                    // native dump (host side). rc==0 counts ACCEPTED/ENQUEUED ONLY — never a
                    // displayed or "ready" claim; independent runtime evidence is the native
                    // 120-byte stats delta written by WriteGpuWindow (gpu-pipeline-10s.json).
                    int grc;
                    try
                    {
                        var wpl = VRSIMPlugin.MakeRawProjectionPlan(pl);
                        var wpr = VRSIMPlugin.MakeRawProjectionPlan(pr);
                        grc = _host.SubmitRawStereo(rawL, rawR, FrameW, FrameH, FrameW * 4,
                            wireSerial, capturedPoseSerial, capturedNativeEpoch, wpl, wpr, _lastYawDelta);
                    }
                    catch (Exception gex)
                    {
                        _submitFailures++;
                        if (_gpuSubmitLogCount < 10)
                        {
                            _gpuSubmitLogCount++;
                            _host.Log("VC_GPU_SUBMIT_EXCEPTION frameSerial=" + wireSerial + " " +
                                gex.GetType().Name + ": " + gex.Message +
                                " logged=" + _gpuSubmitLogCount + "/10");
                        }
                        return;
                    }
                    long gpuAcceptedQpc = Stopwatch.GetTimestamp();
                    if (grc == 0)
                    {
                        _framesSubmitted++;
                        _liveSubmitted++;
                        NoteGpuEnqueueSuccess(captureStartQpc, gpuAcceptedQpc, capturePairMs);
                        // Telemetry profiling collection AFTER the existing counters/Note, on the accepted
                        // pair ONLY. A missing/mismatched timing records telemetry-missing and never
                        // fails or changes the already-committed submit.
                        if (hostedPairTiming.HasValue &&
                            hostedPairTiming.Value.OriginalGeneration == captureGeneration &&
                            captureGeneration == _hostedStartGeneration)
                        {
                            var acceptedHt = hostedPairTiming.Value;
                            acceptedHt.CapturePairMs = capturePairMs;   // SAME pair, SAME tick — never re-measured
                            // Non-finite rows are counted once but still kept as pure numeric rows;
                            // AppendPercentiles drops invalid values per column. Never fake 0, never throw.
                            if (!HasFiniteHostedTiming(acceptedHt)) _gpuHostedTimingMissingOrInvalid++;
                            _gpuHostedTimings.Add(acceptedHt);
                            TrimAsyncWindowList(_gpuHostedTimings);
                        }
                        else if (_hostedBackendSelected)
                        {
                            // Only the hosted source can have a "missing hosted timing"; legacy
                            // non-hosted GPU submits are not counted as missing hosted telemetry.
                            _gpuHostedTimingMissingOrInvalid++;
                        }
                    }
                    else
                    {
                        _submitFailures++;
                        if (_gpuSubmitLogCount < 10)
                        {
                            _gpuSubmitLogCount++;
                            _host.Log("VC_GPU_SUBMIT_FAIL frameSerial=" + wireSerial +
                                " poseSerial=" + capturedPoseSerial +
                                " epoch=" + capturedNativeEpoch + " rc=" + grc +
                                " logged=" + _gpuSubmitLogCount + "/10");
                        }
                    }
                    return;   // GPU branch ends the tick BEFORE the CPU enqueue path
                }
                StoreCaptureMeta(new CaptureMetaRow(wireSerial, _poseSerialUsed, _generation,
                    P_Lcam, capL, capR, captureStartQpc, capturePairMs));
                var worker = _worker;
                if (worker == null || !worker.TrySubmit(new StereoResampleJob(
                        _generation, wireSerial, _poseSerialUsed, rawL, rawR, pl, pr)))
                {
                    // A rejected job can never complete — drop its metadata immediately and log bounded.
                    RemoveMeta(wireSerial);
                    if (_jobRejectLogCount < 10)
                    {
                        _jobRejectLogCount++;
                        _host.Log("VC_ASYNC_JOB_REJECTED frameSerial=" + wireSerial +
                            " poseSerial=" + _poseSerialUsed +
                            " worker=" + (worker == null ? "null" : "present") +
                            " logged=" + _jobRejectLogCount + "/10");
                    }
                }
            }
            catch (Exception ex)
            {
                // Failed tick: log honestly. The old synchronous StageSampler accounting and the
                // performance-stages-10s.json emit are REMOVED — async completion telemetry goes
                // to async-pipeline-10s.json (next card).
                // NC02-XR-P5: staging-mode unexpected exceptions converge through the ORIGINAL
                // owner/generation — never just log and keep using an old ticket. The OFF log
                // path below stays verbatim.
                if ((_nc02StagingRequested || _obsSharedRequested) && _running && _generation == nc02OriginalGeneration &&
                    nc02OriginalOwner != null &&
                    nc02OriginalOwner.BoundGeneration == nc02OriginalGeneration)
                {
                    RecordHostedCaptureFailure(nc02OriginalGeneration,
                        (_obsSharedRequested ? "obs_shared_tick_exception " : "nc02_tick_exception ") + ex.GetType().Name + ": " + ex.Message);
                    return;
                }
                _host.Log("PRODUCER_TICK_FAIL " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        /// <summary>Managed convergence for a REAL hosted fault/timeout (category B ONLY —
        /// explicit Stop / generation invalidation must never route here). Saves the original
        /// Start/owner-bound generation and the concrete reason, closes the timer and the running
        /// state, invalidates the producer generation, and closes the owner's managed gate via
        /// RequestStop(generation) — pure managed, owner RETAINED. NO legacy Dispose, NO native
        /// Stop/Destroy, NO fallback and NO automatic rebootstrap. The notification-allowed flag
        /// is consumed only by the later async-notification cards; an explicit Stop (which leaves
        /// _stopStatus "stopped") cancels any stale notification.</summary>
        private void RecordHostedCaptureFailure(int generation, string reason)
        {
            _hostedFailureGeneration = generation;
            _hostedFailureReason = string.IsNullOrEmpty(reason) ? "unspecified" : reason;
            _hostedFailureNotifyAllowed = true;
            _running = false;
            if (_timer != null)
            {
                try { _timer.Stop(); } catch { }
                _timer = null;
            }
            // OBS-SHARED (Card3B1) first-failure freeze — BEFORE the _generation++ invalidation, the
            // SAME pure-managed/native-metadata helper the explicit Stop path uses. No Discard,
            // Finalize or Confirm happens while this failed capture is still on the entered-tick /
            // finally stack; the bounded finite source steps run only after unwind, on the host
            // deferred retry. Every packet/ref and the retained capture fault stay untouched.
            FreezeObsSharedFirstStopOnce();
            _generation++;   // FIRST invalidation — nothing may capture/submit for this generation
            _stopStatus = "capture_failed";
            // OBS-SHARED: same-call admission close — RequestStop against the frozen POSITIVE
            // original generation (pre-Begin RequestStop binds/closes; the resulting positive
            // BoundGeneration is verified) plus the metadata-only drain request, BEFORE any new
            // submit can be attempted. The generic owner gate close below stays for non-shared modes.
            if (_obsSharedRequested)
            {
                CloseObsSharedFirstStopAdmissionOnce();
                _host.Log("VC_PRODUCER obs_shared_capture_failure_closed generation=" +
                    _obsSharedStopBoundGeneration.ToString(CultureInfo.InvariantCulture) +
                    " requestStop_and_drain_same_call=true captureFaultRetained=true");
            }
            var o = _hostedOwner;
            if (o != null && !_obsSharedRequested)
            {
                try { o.RequestStop(generation); }
                catch (Exception rex)
                {
                    _host.Log("VC_PRODUCER hosted_failure_gate_close_fail " + rex.GetType().Name + ": " + rex.Message);
                }
            }
            _host.Log("VC_PRODUCER hosted_capture_failed generation=" + generation +
                " reason=" + _hostedFailureReason +
                " ownerRetained=true no_fallback no_auto_rebootstrap");
            // Coalesced-background opt-in: cancel this token's PENDING pulse Operation. An
            // EXECUTING pulse cannot be aborted and KEEPS its token until its own finally releases
            // it — the in-flight capture stays governed by the existing _tickInProgress / owner
            // StopVerified gates. NO new NativeStop/Destroy here; a finished old callback can never
            // resurrect the timer or a new generation (its token/current-gen checks fail).
            CancelPendingHostedPulse();
            // Kernel-pulse opt-in: non-blocking ASYNC stop request only. Pulse/task/owner refs stay
            // retained so the host's exact-source 100 ms StopAndRestore retry can verify the stop
            // before any StopHostedCore/SDK cleanup — no wait, no Result read, no native here.
            RequestKernelPulseStop();
        }

        // ---------------- coalesced-background capture scheduler (opt-in, UI-thread ONLY) ----------------
        // Pure managed scheduling: the existing Tick (and ONLY the existing Tick) enters the SDK
        // owner transaction. No 0 ms DispatcherTimer, no Thread.Sleep, no busy-wait/SpinWait, no
        // second capture thread; the control helpers below touch ONLY managed gates/owner properties
        // already used by the reviewed hosted paths.

        /// <summary>Reservation token binding ONE coalesced pulse to its originating hosted
        /// generation and the retained owner reference. Token identity itself is the claim: the
        /// slot is released only by a ReferenceEquals-exact compare-exchange.</summary>
        private sealed class HostedPulseToken
        {
            public HostedPulseToken(int generation, NC04.NC01HostedSceneOwner owner)
            {
                Generation = generation;
                Owner = owner;
            }
            public readonly int Generation;
            public readonly NC04.NC01HostedSceneOwner Owner;
            public DispatcherOperation Operation;
            public volatile bool Executing;
        }

        private void CancelPendingHostedPulse()
        {
            var token = _coalescedPulseToken;
            if (token == null || token.Executing) return;   // running callback keeps its token until its own finally
            var op = token.Operation;
            if (op == null) return;   // BeginInvoke not recorded yet — never blindly clear: the
                                      // BeginInvoke-failure catch or the callback finally cleans up
            if (op.Status != System.Windows.Threading.DispatcherOperationStatus.Pending) return;
            if (!op.Abort()) return;   // callback already started — leave the token to that callback
            // Abort confirmed the callback will never run: release ONLY this token's own claim.
            System.Threading.Interlocked.CompareExchange(ref _coalescedPulseToken, null, token);
        }

        /// <summary>First-pulse gate from the outermost Tick finally: opt-in + original gen valid +
        /// running + not inside a tick + PURE-MANAGED owner gates only (admission, backend-ready
        /// flags, no sticky failure, EntryLocked). No SDK transaction here; the only SDK
        /// validation stays in TickCore. Any unmet gate silently leaves the retained timer as the
        /// driver.</summary>
        private void TryScheduleHostedPulseFromTimer()
        {
            int gen = _hostedStartGeneration;
            var owner = _hostedOwner;
            if (!_coalescedBackgroundScheduler || !_running || gen < 0 || owner == null ||
                _generation != gen || owner.BoundGeneration != gen || _tickInProgress ||
                !CaptureAdmissionOpen || !IsCaptureBackendReady ||
                !string.IsNullOrEmpty(CaptureBackendFailureReason) ||
                _cal == null || _cal.State != EntryCalState.EntryLocked)
                return;
            if (!ReferenceEquals(owner, _hostedOwner)) return;
            TryScheduleHostedPulse(gen, owner);
        }

        /// <summary>Claim the SINGLE token slot BEFORE anything else; at most one pending/executing
        /// pulse per producer instance. On a successful claim the retained original timer is
        /// STOPPED (reference kept — never a new timer) so the handoff produces no empty Render
        /// beats. UI-thread Background priority (below Input/Render — never Send). A BeginInvoke
        /// failure releases only this token's own claim and restarts the retained timer when the
        /// original generation/owner/timer are still valid; it is logged.</summary>
        private void TryScheduleHostedPulse(int generation, NC04.NC01HostedSceneOwner owner)
        {
            if (!_coalescedBackgroundScheduler) return;
            var token = new HostedPulseToken(generation, owner);
            if (System.Threading.Interlocked.CompareExchange(ref _coalescedPulseToken, token, null) != null)
                return;   // one pending/executing pulse only
            var timer = _timer;
            bool timerStopped = false;
            if (timer != null)
            {
                try { timer.Stop(); timerStopped = true; } catch { }
            }
            try
            {
                token.Operation = _disp.BeginInvoke(DispatcherPriority.Background,
                    (System.Action)(() => HostedPulseCallback(token)));
            }
            catch (Exception ex)
            {
                System.Threading.Interlocked.CompareExchange(ref _coalescedPulseToken, null, token);
                if (timerStopped && _running && _hostedStartGeneration == generation &&
                    _generation == generation && ReferenceEquals(_hostedOwner, owner) &&
                    owner.BoundGeneration == generation)
                {
                    try { timer.Start(); } catch { }   // restore the SAME retained timer as the driver
                }
                _host.Log("VC_PRODUCER hosted_pulse_schedule_fail generation=" + generation +
                    " " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        /// <summary>The single retained Background pulse. Executing is marked at entry, before any
        /// gate, so a concurrent cancel can never clear or abort this callback's claim. Revalidates
        /// token identity, current generation, retained owner reference, running, no reentered tick
        /// and PURE-MANAGED gates (admission, backend-ready flags, no failure, EntryLocked) before
        /// calling the EXISTING Tick — the token stays claimed across Tick and all of its finallys,
        /// so the SDK pump can never queue the same frame twice. Unexpected exceptions never escape
        /// to WPF and never silently continue: while the token's original generation is still valid
        /// they converge through the existing RecordHostedCaptureFailure(original gen, concrete
        /// reason). ALL recovery lives in the finally (a try-side return must not skip it): it
        /// releases ONLY its own token (ReferenceEquals compare-exchange) and, only when the claim
        /// was still ours and bound to the original generation/owner with no stop/fault, queues
        /// exactly ONE next Background pulse after a live acceptance on this tick — otherwise the
        /// retained timer is restored as the backoff driver, so a tracked/epoch/queue-style
        /// fast-return can never re-pulse into a busy loop. A foreign/cleared token touches
        /// nothing.</summary>
        private void HostedPulseCallback(HostedPulseToken token)
        {
            int tokenGeneration = token.Generation;
            var tokenOwner = token.Owner;
            bool ranTick = false;
            ulong liveBefore = _liveSubmitted;
            try
            {
                token.Executing = true;   // before ANY gate — cancel defers to this finally from now on
                var owner = _hostedOwner;
                int gen = _hostedStartGeneration;
                if (!ReferenceEquals(_coalescedPulseToken, token) || !_coalescedBackgroundScheduler)
                    return;   // foreign/cleared token — never touch anything
                if (!_running || gen < 0 || gen != tokenGeneration || owner == null ||
                    !ReferenceEquals(owner, tokenOwner) || _generation != gen || _tickInProgress ||
                    owner.BoundGeneration != gen || !owner.AdmissionOpen ||
                    !string.IsNullOrEmpty(owner.FailureReason) ||
                    !IsCaptureBackendReady ||
                    _cal == null || _cal.State != EntryCalState.EntryLocked)
                    return;   // stopped/rebound/busy/not-locked — finally restores the retained timer
                liveBefore = _liveSubmitted;
                _coalescedPulseOrigin = true;
                ranTick = true;
                Tick();
            }
            catch (Exception ex)
            {
                ranTick = false;   // a faulted pulse never chains the next pulse
                if (_running && _hostedStartGeneration == tokenGeneration && _generation == tokenGeneration &&
                    ReferenceEquals(_hostedOwner, tokenOwner) && tokenOwner.BoundGeneration == tokenGeneration)
                {
                    try
                    {
                        RecordHostedCaptureFailure(tokenGeneration,
                            "hosted_coalesced_pulse_exception " + ex.GetType().Name + ": " + ex.Message);
                    }
                    catch (Exception rex)
                    {
                        _host.Log("VC_PRODUCER hosted_coalesced_failure_converge_fail " +
                            rex.GetType().Name + ": " + rex.Message);
                    }
                }
            }
            finally
            {
                token.Executing = false;
                _coalescedPulseOrigin = false;   // defensive: the origin flag must not outlive this pulse
                // Release ONLY this token (ReferenceEquals semantics): an old token can never clear
                // a newer token or open a new-generation timer.
                bool ownsClaim = ReferenceEquals(_coalescedPulseToken, token);
                if (ownsClaim)
                {
                    System.Threading.Interlocked.CompareExchange(ref _coalescedPulseToken, null, token);
                    var ownerNow = _hostedOwner;
                    bool stillValid = _coalescedBackgroundScheduler && _running &&
                        _hostedStartGeneration == tokenGeneration &&
                        _generation == tokenGeneration && ownerNow != null &&
                        ReferenceEquals(ownerNow, tokenOwner) &&
                        ownerNow.BoundGeneration == tokenGeneration &&
                        ownerNow.AdmissionOpen && string.IsNullOrEmpty(ownerNow.FailureReason);
                    if (stillValid)
                    {
                        if (ranTick && _liveSubmitted != liveBefore)
                        {
                            TryScheduleHostedPulse(tokenGeneration, tokenOwner);   // exactly ONE next Background pulse
                        }
                        else
                        {
                            // No live acceptance this tick (or a gate-return/early return): restore
                            // the RETAINED timer as the backoff driver — never re-pulse immediately.
                            // A null/foreign timer is never created or resurrected here.
                            var t = _timer;
                            if (t != null && _generation == tokenGeneration)
                            {
                                try { t.Start(); } catch { }
                            }
                        }
                    }
                }
            }
        }

        // Queues AT MOST ONE Dispatcher notification per failure generation, only from a
        // non-capturing context (Tick finally after the SDK transaction exited). The callback
        // revalidates allowed + pending + same gen/reason + owned start generation + idle
        // tick/owner before reporting; a still-busy owner only releases the queued marker so
        // the next Tick finally can retry — the host never sees the same fault twice.
        private void TryQueueHostedFailureNotification()
        {
            if (!_hostedBackendSelected) return;
            if (!HostedCaptureFailureNotificationPending) return;
            if (_tickInProgress) return;
            var o = _hostedOwner;
            if (o == null || o.CaptureBusy) return;
            int gen = _hostedFailureGeneration;
            if (gen < 0) return;
            if (gen == _hostedFailureNotifyQueuedGeneration || gen == _hostedFailureNotifySentGeneration) return;
            string reason = _hostedFailureReason;
            _hostedFailureNotifyQueuedGeneration = gen;
            try
            {
                _disp.BeginInvoke(new System.Action(() =>
                {
                    try
                    {
                        if (!_hostedFailureNotifyAllowed) return;
                        if (!HostedCaptureFailureNotificationPending) return;
                        if (_hostedFailureGeneration != gen || _hostedFailureReason != reason) return;
                        if (_hostedStartGeneration != gen) return;
                        if (_tickInProgress) return;
                        var own = _hostedOwner;
                        if (own == null) return;
                        if (own.CaptureBusy)
                        {
                            if (_hostedFailureNotifySentGeneration != gen)
                                _hostedFailureNotifyQueuedGeneration = -1;
                            return;
                        }
                        if (_hostedFailureNotifySentGeneration == gen) return;
                        _hostedFailureNotifySentGeneration = gen;
                        _host.ReportHostedCaptureFailure(this, gen, reason);
                    }
                    catch (Exception cex)
                    {
                        _hostedFailureNotifyQueuedGeneration = -1;
                        _host.Log("VC_PRODUCER hosted_failure_notify_callback_error " +
                            cex.GetType().Name + ": " + cex.Message);
                    }
                }));
            }
            catch (Exception qex)
            {
                _hostedFailureNotifyQueuedGeneration = -1;
                _host.Log("VC_PRODUCER hosted_failure_notify_queue_fail " +
                    qex.GetType().Name + ": " + qex.Message);
            }
        }

        /// <summary>Pre-origin frame: the entry camera captured ONCE and cached, then
        /// resubmitted every tick (0.1.3.6). This is the STATIC entry preview shown while
        /// EntryCalibration collects its 1.5 s stability window — explicitly NOT a live
        /// stereo/realtime image (no per-eye cameras, no reprojection claim; PHASE 21/22
        /// gate). Capture keeps the DESKTOP symmetric entry FOV; we do NOT claim the image
        /// rays match the XR per-eye asymmetric frustum before calibration. After
        /// ENTRY_LOCKED the cache is dropped and live per-eye capture/projection runs.</summary>
        private void SubmitEntryFrame()
        {
            // OBS-SHARED defensive return: shared never runs the CPU static entry preview.
            if (_obsSharedRequested) return;
            // Hosted route: the legacy single-frame Capture(_entryView)/NativeCaptureOwner path is
            // FORBIDDEN — the hosted branch below never calls Capture() and _nativeOwner stays null.
            if (_hostedBackendSelected)
            {
                SubmitHostedEntryPreview();
                return;
            }
            try
            {
                if (_preCalibrationFrame == null)
                {
                    byte[] f = Capture(_entryView);
                    ForceAlpha(f);   // cache is immutable from here (opaque)
                    _preCalibrationFrame = f;
                    _host.Log("PRECALIBRATION_STATIC_ENTRY_FRAME cached — entry-view preview during calibration, NOT stereo/live");
                }
                // STATIC preview by design: the SAME cached frame every tick, poseSerial 0 — it is
                // never live content, never enters async telemetry, and counts FramesSubmitted
                // ONLY on an actual rc==0 submit.
                _frameSerial++;
                int rc = _host.SubmitStereo(_preCalibrationFrame, _preCalibrationFrame, FrameW, FrameH, FrameW * 4, _frameSerial, 0);
                if (rc == 0) _framesSubmitted++;
                else _host.Log("SUBMIT_ENTRY_FAIL rc=" + rc + " frameSerial=" + _frameSerial);
            }
            catch (Exception ex)
            {
                _host.Log("SUBMIT_ENTRY_FAIL " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        /// <summary>Clears BOTH hosted static raw-preview caches. Fully isolated from the legacy
        /// single _preCalibrationFrame path — neither cache ever feeds or replaces the other.</summary>
        private void ClearHostedPreviewCaches()
        {
            _hostedPreviewLeft = null;
            _hostedPreviewRight = null;
            _hostedPreviewFovL = double.NaN;
            _hostedPreviewFovR = double.NaN;
            _hostedPreviewAspect = double.NaN;
            _hostedPreviewExtentW = 0;
            _hostedPreviewExtentH = 0;
            _hostedPreviewOrtho = false;
        }

        /// <summary>hosted-d3d9 pre-calibration STATIC raw preview (reviewed07). Reachable only after
        /// the Tick hosted readiness prefix — an opt-in can NEVER preview before an actual hosted
        /// Ready. First call: ONE CaptureRawPair(_entryView, _entryView, _entryFovY, _entryFovY, 1.0,
        /// true, ORIGINAL generation, shouldStop) — the raw entry view is passed directly, NEVER
        /// Inversed; both actual eyes share the SAME entry pose (an honest static raw preview, NOT
        /// ray-aligned and NOT a real XR stereo/live-content claim) while the owner still performs two
        /// INDEPENDENT per-eye SurfaceRender+readback transactions — a second-eye failure yields no
        /// partial cache and no submit. The 06 managed pair validation (actual matrices/projection/
        /// info/dims/main snapshots) runs before anything is kept; the payloads are then CLONED into
        /// private caches and alpha-255 normalized on the CLONES only — the owner's raw return buffers
        /// are never mutated. Cancel checks land BEFORE the owner call, AFTER the return, BEFORE the
        /// cache commit and BEFORE the submit: a Stop/generation invalidation caches nothing and
        /// submits nothing. Real failures converge through the 05 managed fault gate — never a
        /// log-only infinite retry. Submit uses the existing CPU static-preview ABI (SubmitStereo,
        /// poseSerial 0); FramesSubmitted counts rc==0 ONLY and the live/native-unique counters never
        /// see a preview frame.</summary>
        private void SubmitHostedEntryPreview()
        {
            // OBS-SHARED defensive return: shared never runs the hosted static raw preview.
            if (_obsSharedRequested) return;
            int originalGen = _hostedStartGeneration;
            var ho = _hostedOwner;
            // Pre-owner gate: still the original running generation, admission open, failure-free,
            // AND an actual hosted Ready — no preview before readiness. Stop/invalidation → silent
            // cancel, never a recorded failure.
            if (!_running || _generation != originalGen ||
                ho == null || ho.BoundGeneration != originalGen ||
                !ho.AdmissionOpen || !string.IsNullOrEmpty(ho.FailureReason) ||
                !IsCaptureBackendReady)
                return;
            try
            {
                bool cached = _hostedPreviewLeft != null && _hostedPreviewRight != null;
                if (!cached)
                {
                    NC04.NC01CaptureSampleV1[] samples = ho.CaptureRawPair(
                        _entryView, _entryView, _entryFovY, _entryFovY, 1.0, true,
                        originalGen, () => !_running || _generation != originalGen);
                    // AFTER the return: an invalidation during the SDK pumping discards the pair —
                    // no validation credit, no cache, no submit, nothing recorded.
                    if (!_running || _generation != originalGen)
                    {
                        _host.Log("VC_HOSTED_PREVIEW_DROP running=" + (_running ? "true" : "false") +
                            " startGeneration=" + originalGen.ToString(CultureInfo.InvariantCulture) +
                            " generation=" + _generation.ToString(CultureInfo.InvariantCulture));
                        return;
                    }
                    string invalid = ValidateHostedRawPair(samples, _entryFovY, _entryFovY);
                    if (invalid != null)
                    {
                        if (_running && _generation == originalGen)
                            RecordHostedCaptureFailure(originalGen, invalid);
                        return;
                    }
                    // BEFORE the cache commit: re-verify the ORIGINAL generation and the open
                    // admission — a stop/cancel racing the validation commits nothing.
                    if (!_running || _generation != originalGen ||
                        ho.BoundGeneration != originalGen || !ho.AdmissionOpen)
                        return;
                    // CLONE into independent private buffers, THEN normalize alpha on the clones —
                    // the owner's original raw return buffers stay untouched.
                    byte[] left = (byte[])samples[0].Bgra.Clone();
                    byte[] right = (byte[])samples[1].Bgra.Clone();
                    ForceAlpha(left);
                    ForceAlpha(right);
                    _hostedPreviewLeft = left;
                    _hostedPreviewRight = right;
                    _hostedPreviewFovL = samples[0].FovY;
                    _hostedPreviewFovR = samples[1].FovY;
                    _hostedPreviewAspect = samples[0].AspectRatio;
                    _hostedPreviewOrtho = samples[0].Ortho;
                    _hostedPreviewExtentW = (int)ReadU32Le(samples[0].SurfaceInfo, 8);
                    _hostedPreviewExtentH = (int)ReadU32Le(samples[0].SurfaceInfo, 12);
                    _host.Log("HOSTED_PRECALIBRATION_STATIC_RAW_PREVIEW cached — ONE CaptureRawPair(_entryView,_entryView)," +
                        " both eyes share the ENTRY pose with independent per-eye render/readback; RAW BGRA clone, NOT ray-aligned," +
                        " NOT real XR stereo, NOT live content");
                }
                // BEFORE the submit: cancel re-check — a stopped/invalidated generation submits nothing.
                if (!_running || _generation != originalGen ||
                    ho.BoundGeneration != originalGen || !ho.AdmissionOpen)
                    return;
                // STATIC preview by design: the SAME cached pair every tick, poseSerial 0, existing
                // CPU SubmitStereo ABI. rc==0 counts FramesSubmitted ONLY — never live/native-unique.
                _frameSerial++;
                int rc = _host.SubmitStereo(_hostedPreviewLeft, _hostedPreviewRight, FrameW, FrameH, FrameW * 4, _frameSerial, 0);
                if (rc == 0) _framesSubmitted++;
                else _host.Log("HOSTED_SUBMIT_ENTRY_FAIL rc=" + rc + " frameSerial=" + _frameSerial.ToString(CultureInfo.InvariantCulture));
            }
            catch (OperationCanceledException)
            {
                // Cancel (explicit Stop / generation invalidation raced the owner call): NOTHING
                // cached, NOTHING submitted, NOTHING recorded as a fault — the stop path closes it.
            }
            catch (Exception ex)
            {
                // A REAL failure converges through the 05 managed fault gate (closes running/timer,
                // invalidates the generation, RequestStops the owner) — never log-only infinite retry.
                if (_running && _generation == originalGen)
                    RecordHostedCaptureFailure(originalGen,
                        "hosted_preview_failed " + ex.GetType().Name + ": " + ex.Message);
            }
        }

        // ---------------- P1 stage sampler -> performance-stages-10s.json ----------------
        /// <summary>02D-P1: per-tick stage samples of SUCCESSFUL live stereo submits only (rolling cap
        /// 300, one JSON per 10s window). Clock: Stopwatch QPC (monotonic). Percentiles are computed
        /// per column over the same-window samples and MUST NOT be summed across columns; the sum≤total
        /// check runs per sample with a small tolerance (unaccountedTickMs kept per sample).</summary>
        private sealed class StageSampler
        {
            // The selected nominal timer interval is passed explicitly at construction — the
            // nested class keeps no implicit link to the outer producer fields.
            private readonly double _nominalMs;

            public StageSampler(double nominalIntervalMs) { _nominalMs = nominalIntervalMs; }

            private struct Sample
            {
                public double CapL, CapR, RsL, RsR, Submit, Total, Restore;
                public double Interval, Excess, Unaccounted;
            }

            private readonly System.Collections.Generic.List<Sample> _samples = new System.Collections.Generic.List<Sample>();
            private long _windowStartQpc;      // 0 = window not started (first successful live pair opens it)
            private long _lastQpc;
            private double _lastTotal;
            private bool _hasLast;
            private int _pairsWindow, _failedWindow, _excludedWindow, _violations;

            private const double ToleranceMs = 1.0;
            private static readonly long WindowQpc = Stopwatch.Frequency * 10;   // 10 s

            public void Reset()
            {
                _samples.Clear(); _windowStartQpc = 0; _hasLast = false;
                _pairsWindow = 0; _failedWindow = 0; _excludedWindow = 0; _violations = 0;
            }

            public void NoteFailedTick() { _failedWindow++; }

            /// <summary>keep=false → non-FOCUSED / tracking-invalid sample剔除 (excluded count only).
            /// The pre-calibration static SubmitEntryFrame never reaches here.</summary>
            public void Add(long qpc, double totalMs, double restoreMs,
                double capL, double capR, double rsL, double rsR, double submitMs, bool keep)
            {
                if (!keep)
                {
                    // fix 3: drop the last-sample anchor across excluded gaps — otherwise the
                    // first sample after e.g. a focus loss would fake a queue excess interval
                    _excludedWindow++; _hasLast = false; return;
                }
                if (_windowStartQpc == 0) _windowStartQpc = qpc;
                var s = new Sample
                {
                    CapL = capL, CapR = capR, RsL = rsL, RsR = rsR,
                    Submit = submitMs, Total = totalMs, Restore = restoreMs
                };
                if (_hasLast)
                {
                    s.Interval = (qpc - _lastQpc) * 1000.0 / Stopwatch.Frequency;
                    // estimate only — NOT a measured dispatcher queue time
                    s.Excess = Math.Max(0.0, s.Interval - _lastTotal - _nominalMs);
                }
                else { s.Interval = double.NaN; s.Excess = double.NaN; }
                s.Unaccounted = totalMs - (capL + capR + rsL + rsR + submitMs);
                if (s.Unaccounted < -ToleranceMs) _violations++;   // per-sample sum<=total+tolerance check
                _samples.Add(s);
                while (_samples.Count > 300) _samples.RemoveAt(0);
                _pairsWindow++;
                _lastQpc = qpc; _lastTotal = totalMs; _hasLast = true;
            }

            public bool ShouldWrite(long qpc)
            {
                return _windowStartQpc != 0 && _pairsWindow > 0 && qpc - _windowStartQpc >= WindowQpc;
            }

            /// <summary>Aggregate the current window, then start a fresh one — old-window counts
            /// are never carried over.</summary>
            public string BuildJson(long qpcNow)
            {
                double elapsed = (qpcNow - _windowStartQpc) / (double)Stopwatch.Frequency;
                int pairs = _pairsWindow, failed = _failedWindow, excluded = _excludedWindow,
                    violations = _violations, count = _samples.Count;
                var ci = CultureInfo.InvariantCulture;
                string j = null;
                if (count > 0)
                {
                    var sb = new StringBuilder();
                    sb.Append("{\"schema\":\"performance-stages-10s/1\"")
                      .Append(",\"frameTimeSource\":\"Stopwatch QPC (monotonic)\"")
                      .Append(",\"scope\":\"successful live stereo-submit ticks only; pre-calibration static SubmitEntryFrame excluded\"")
                      .Append(",\"unknowns\":{\"monitorCost\":\"UNKNOWN\"}")
                      .Append(",\"capturePath\":\"NativeCaptureOwner single request/readback per pose; hash polling removed\"")
                      .Append(",\"elapsedSeconds\":").Append(elapsed.ToString("0.000", ci))
                      .Append(",\"successfulLivePairsWindow\":").Append(pairs)
                      .Append(",\"failedTicksWindow\":").Append(failed)
                      .Append(",\"excludedSamplesWindow\":").Append(excluded)
                      .Append(",\"windowContainsExclusions\":").Append(excluded > 0 ? "true" : "false")
                      .Append(",\"sampleCount\":").Append(count)
                      .Append(",\"contentThroughputFps\":").Append((pairs / Math.Max(1e-9, elapsed)).ToString("0.00", ci))
                      .Append(",\"stages\":{");
                    bool first = true;
                    AppendCol(sb, ci, ref first, "captureLeftMs", v => v.CapL);
                    AppendCol(sb, ci, ref first, "captureRightMs", v => v.CapR);
                    AppendCol(sb, ci, ref first, "resampleLeftMs", v => v.RsL);
                    AppendCol(sb, ci, ref first, "resampleRightMs", v => v.RsR);
                    AppendCol(sb, ci, ref first, "submitPairMs", v => v.Submit);
                    AppendCol(sb, ci, ref first, "restoreMs", v => v.Restore);
                    AppendCol(sb, ci, ref first, "tickTotalMs", v => v.Total);
                    AppendCol(sb, ci, ref first, "unaccountedTickMs", v => v.Unaccounted);
                    sb.Append("},\"pacing\":{\"nominalIntervalMs\":").Append(_nominalMs.ToString("0.00", ci))
                      .Append(",\"queueMeasured\":false,")
                      .Append("\"note\":\"actualIntervalMs from QPC tick stamps; excessIntervalMs=max(0,actualIntervalMs-previousTickMs-selectedNominalIntervalMs) is an ESTIMATE, not a measured dispatcher queue\"},\"pacingStats\":{");
                    first = true;
                    AppendCol(sb, ci, ref first, "actualIntervalMs", v => v.Interval);
                    AppendCol(sb, ci, ref first, "excessIntervalMs", v => v.Excess);
                    sb.Append("},\"sanity\":{\"perSampleSumToleranceMs\":1,\"perSampleViolations\":").Append(violations)
                      .Append(",\"percentilesArePerColumnIndependentNoSum\":true}");
                    sb.Append('}');
                    j = sb.ToString();
                }
                _samples.Clear(); _windowStartQpc = qpcNow; _hasLast = false;
                _pairsWindow = 0; _failedWindow = 0; _excludedWindow = 0; _violations = 0;
                return j;
            }

            private void AppendCol(StringBuilder sb, CultureInfo ci, ref bool first, string name, Func<Sample, double> f)
            {
                var vals = new double[_samples.Count];
                int n = 0;
                for (int i = 0; i < _samples.Count; i++)
                {
                    double v = f(_samples[i]);
                    if (!double.IsNaN(v) && !double.IsInfinity(v)) vals[n++] = v;
                }
                if (n == 0) return;
                Array.Sort(vals, 0, n);
                double p50 = vals[Math.Min(n - 1, (int)(n * 0.50))];
                double p95 = vals[Math.Min(n - 1, (int)(n * 0.95))];
                if (!first) sb.Append(',');
                first = false;
                sb.Append('"').Append(name).Append("\":{\"p50\":").Append(p50.ToString("0.00", ci))
                  .Append(",\"p95\":").Append(p95.ToString("0.00", ci))
                  .Append(",\"max\":").Append(vals[n - 1].ToString("0.00", ci))
                  .Append(",\"n\":").Append(n).Append('}');
            }
        }

        private void SaveMotionEvidence(byte[] left, double[] pLcam)
        {
            // pose-A = first frame; pose-B = first frame after head moved >30mm (PHASE 24)
            if (_poseALeft == null)
            {
                _poseALeft = left;
                _poseAPos = (double[])pLcam.Clone();
                _poseAHash = Hash(left);
                SavePng("pose-A-left.png", left);
                _host.Log("MOTION_EVIDENCE pose-A saved");
                return;
            }
            if (_poseAPos != null && !_poseBSaved && _frameSerial > 20)
            {
                double dd = Math.Sqrt(Sq(pLcam[0] - _poseAPos[0]) + Sq(pLcam[1] - _poseAPos[1]) + Sq(pLcam[2] - _poseAPos[2]));
                if (dd > 30.0)
                {
                    _poseBSaved = true;
                    SavePng("pose-B-left.png", left);
                    WriteJson("motion-validation.json", "{" +
                        "\"poseAPosMm\":[" + _poseAPos[0].ToString("0.0", CultureInfo.InvariantCulture) + "," + _poseAPos[1].ToString("0.0", CultureInfo.InvariantCulture) + "," + _poseAPos[2].ToString("0.0", CultureInfo.InvariantCulture) + "]," +
                        "\"poseBPosMm\":[" + pLcam[0].ToString("0.0", CultureInfo.InvariantCulture) + "," + pLcam[1].ToString("0.0", CultureInfo.InvariantCulture) + "," + pLcam[2].ToString("0.0", CultureInfo.InvariantCulture) + "]," +
                        "\"distanceMm\":" + dd.ToString("0.0", CultureInfo.InvariantCulture) + "," +
                        "\"hashChanged\":" + (_poseAHash != Hash(left) ? "true" : "false") + "," +
                        "\"note\":\"A != B proves live re-render, not a frozen frame\"}");
                    _host.Log("MOTION_EVIDENCE pose-B saved dist=" + dd.ToString("0.0", CultureInfo.InvariantCulture) + "mm");
                }
            }
        }

        // PHASE 20: frame-pacing CSV (interval, pair ms) for bootstrap performance audit
        private DateTime _lastPaceAt = DateTime.MinValue;
        private void PacingRow(ulong serial, ulong poseSerial, int rc)
        {
            try
            {
                var now = DateTime.UtcNow;
                double interval = _lastPaceAt == DateTime.MinValue ? 0 : (now - _lastPaceAt).TotalMilliseconds;
                _lastPaceAt = now;
                var ci = CultureInfo.InvariantCulture;
                string line = now.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture) + "," +
                    serial + "," + poseSerial + "," + interval.ToString("0.0", ci) + "," +
                    _pairMs.ToString("0.0", ci) + "," + _renderMs.ToString("0.0", ci) + "," +
                    _copyMs.ToString("0.00", ci) + "," + rc;
                File.AppendAllText(Path.Combine(_evidenceDir, "frame-pacing.csv"),
                    (new FileInfo(Path.Combine(_evidenceDir, "frame-pacing.csv")).Length == 0
                        ? "utc,frameSerial,poseSerial,intervalMs,pairMs,captureMs,deepCopyMs,submitRc" : "") + line);
            }
            catch { }
        }

        private void LogHeadDeltas(double[] qHead)
        {
            // throttled yaw/pitch delta evidence (PHASE 15/16/17)
            double[] fNow = Apply3(QToMatrix(qHead), new[] { 0.0, 0.0, -1.0 });
            double[] fEntry = Apply3(QToMatrix(_qHead0), new[] { 0.0, 0.0, -1.0 });
            double yawNow = Math.Atan2(fNow[0], fNow[1]);
            double yawEntry = Math.Atan2(fEntry[0], fEntry[1]);
            double pitchNow = Math.Atan2(fNow[2], Math.Sqrt(fNow[0] * fNow[0] + fNow[1] * fNow[1]));
            double pitchEntry = Math.Atan2(fEntry[2], Math.Sqrt(fEntry[0] * fEntry[0] + fEntry[1] * fEntry[1]));
            _lastYawDelta = (yawNow - yawEntry) * 180.0 / Math.PI;
            _lastPitchDelta = (pitchNow - pitchEntry) * 180.0 / Math.PI;
        }

        internal void WriteStats(StringBuilder sb)
        {
            var owner = _nativeOwner;
            ulong captureCount = owner != null ? owner.CaptureCount : 0UL;
            bool actualKnown = owner != null && owner.Begun && captureCount > 0;
            sb.Append(",\"vcProducer\":{\"frameSerial\":").Append(_frameSerial)
              .Append(",\"poseSerialUsed\":").Append(_poseSerialUsed)
              .Append(",\"cameraChangeCount\":").Append(_cameraChangeCount)
              .Append(",\"invalidTrackingFrames\":").Append(_invalidTrackingFrames)
              .Append(",\"renderAttempts\":").Append(_renderAttempts)
              .Append(",\"renderSuccess\":").Append(_renderSuccess)
              .Append(",\"lastPairMs\":").Append(_pairMs.ToString("0.0", CultureInfo.InvariantCulture))
              .Append(",\"lastYawDeltaDeg\":").Append(_lastYawDelta.ToString("0.00", CultureInfo.InvariantCulture))
              .Append(",\"lastPitchDeltaDeg\":").Append(_lastPitchDelta.ToString("0.00", CultureInfo.InvariantCulture))
              .Append(",\"originCaptured\":").Append(_originCaptured ? "true" : "false")
              .Append(",\"freshnessUnconfirmed\":").Append(_freshnessUnconfirmed)
              .Append(",\"eyeFovSkips\":").Append(_fovInvalidSkips)
              .Append(",\"selectedConfig\":{\"profile\":\"").Append(CaptureProfile.Selected)
              .Append("\",\"width\":").Append(FrameW)
              .Append(",\"height\":").Append(FrameH)
              .Append(",\"payloadBytes\":").Append(CaptureProfile.PayloadBytes).Append("}")
              .Append(",\"ownerActual\":{\"begun\":").Append(owner != null && owner.Begun ? "true" : "false")
              .Append(",\"captureCount\":").Append(captureCount);
            if (actualKnown)
            {
                sb.Append(",\"width\":").Append(owner.LastActualWidth)
                  .Append(",\"height\":").Append(owner.LastActualHeight)
                  .Append(",\"payloadBytes\":").Append(owner.LastPayloadBytes);
            }
            else
            {
                sb.Append(",\"width\":\"unknown\",\"height\":\"unknown\",\"payloadBytes\":\"unknown\"");
            }
            sb.Append("}")
              .Append(",\"mode\":\"02BC\"");
            if (_hostedBackendSelected) AppendHostedStats(sb);
            sb.Append('}');
        }

        /// <summary>Minimal hosted-d3d9 stats block (card 08 — deliberately NOT a GetStats field
        /// refactor): backend / pending / ready / fault / closed, the owner's pure-managed
        /// qualityGeneration / captureBusy / stopVerified, the Stop bookkeeping generations, and the
        /// actual projection/extent VALUES COPIED from the cached preview pair at capture time. No SDK
        /// object is read here and none enters the background JSON; detailed per-call metrics stay in
        /// the existing QA files. QPC helper durations elsewhere remain CPU-side timings — never GPU
        /// time — and the flushes above follow the existing GPU telemetry windows. accepted(rc==0
        /// enqueues) and successful live layers stay distinct: the static preview only ever counts
        /// FramesSubmitted; live/native-unique counters never see it.</summary>
        private void AppendHostedStats(StringBuilder sb)
        {
            var ho = _hostedOwner;
            var ci = CultureInfo.InvariantCulture;
            bool previewCached = _hostedPreviewLeft != null && _hostedPreviewRight != null;
            string failure = _hostedStartGeneration >= 0 && _hostedFailureGeneration == _hostedStartGeneration
                ? _hostedFailureReason
                : (ho == null ? string.Empty : (ho.FailureReason ?? string.Empty));
            sb.Append(",\"hosted\":{\"backend\":\"hosted-d3d9\"")
              .Append(",\"startGeneration\":").Append(_hostedStartGeneration.ToString(ci))
              .Append(",\"pendingReady\":").Append(_running && !IsCaptureBackendReady ? "true" : "false")
              .Append(",\"ready\":").Append(IsCaptureBackendReady ? "true" : "false")
              .Append(",\"faultRecorded\":").Append(!string.IsNullOrEmpty(failure) ? "true" : "false")
              .Append(",\"failureReason\":\"").Append(JsonSafe(failure)).Append('"')
              .Append(",\"closed\":").Append(ho != null && ho.StopVerified ? "true" : "false")
              .Append(",\"stopPending\":").Append(_hostedStopPending ? "true" : "false")
              .Append(",\"stopBoundGeneration\":").Append(_hostedStopBoundGeneration.ToString(ci))
              .Append(",\"qualityGeneration\":").Append((ho != null ? ho.QualityGeneration : -1).ToString(ci))
              .Append(",\"captureBusy\":").Append(ho != null && ho.CaptureBusy ? "true" : "false")
              .Append(",\"previewCached\":").Append(previewCached ? "true" : "false");
            if (previewCached)
            {
                sb.Append(",\"previewProjection\":{\"fovYDegLeft\":").Append(JsonNumber(_hostedPreviewFovL))
                  .Append(",\"fovYDegRight\":").Append(JsonNumber(_hostedPreviewFovR))
                  .Append(",\"aspect\":").Append(JsonNumber(_hostedPreviewAspect))
                  .Append(",\"ortho\":").Append(_hostedPreviewOrtho ? "true" : "false")
                  .Append("},\"previewActualExtent\":{\"width\":").Append(_hostedPreviewExtentW.ToString(ci))
                  .Append(",\"height\":").Append(_hostedPreviewExtentH.ToString(ci)).Append('}');
            }
            sb.Append(",\"framesSubmittedNote\":\"hosted static raw preview counts FramesSubmitted on rc==0 only; never counted as live or native-unique content\"")
              .Append(",\"windowsRetainedByDesign\":true")
              .Append(",\"telemetryNote\":\"stopPending/closed reflect this owner's logical SDK transactions only; official rendering continues independently; owner references retained until the fresh process ends\"}");
        }

        /// <summary>JSON-safe single-line scalar: quotes/backslashes become '_', control characters
        /// become spaces — exception messages embedded in failureReason can never break the JSON.</summary>
        private static string JsonSafe(string s)
        {
            if (string.IsNullOrEmpty(s)) return string.Empty;
            var sb = new StringBuilder(s.Length + 8);
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (c == '"' || c == '\\') sb.Append('_');
                else if (c < ' ') sb.Append(' ');
                else sb.Append(c);
            }
            return sb.ToString();
        }

        // ---------------- capture: delegated to NativeCaptureOwner (sole recording owner) ----------------

        /// <summary>Capture at the given raw world-to-camera view. The owner performs ONE native
        /// request/readback per pose and returns its exact private BGRX buffer; a single readback
        /// is NOT by itself pose-image binding proof — identical bytes for an identical (or
        /// static-scene) pose are legitimate, and head tracking / stereo binding are evaluated
        /// separately, not here.</summary>
        private byte[] Capture(Matrix pose) { return Capture(pose, double.NaN); }

        /// <summary>fovYDeg = per-eye symmetric SOURCE FieldOfViewY (02D reprojection plan);
        /// NaN → owner leaves the camera FOV untouched.</summary>
        private byte[] Capture(Matrix pose, double fovYDeg)
        {
            var owner = _nativeOwner;
            if (owner == null || !owner.Begun)
                throw new InvalidOperationException("capture owner missing or not begun");
            var frame = owner.Capture(pose, fovYDeg);
            ForceAlpha(frame);
            return frame;
        }

        /// <summary>LIVE paired capture: ONE guarded owner.CaptureStereo request for both eyes.
        /// Owner missing/notBegun and a malformed pair (null array / wrong arity / null or
        /// wrong-length payloads) THROW — the failed tick never counts a render success and
        /// never enqueues a job. Alpha normalization deliberately runs in the caller so the
        /// pair timing covers it. Single-Capture semantics are unchanged.
        /// hosted-d3d9 branch (method head, checked FIRST): the same WORLD-TO-CAMERA raw views
        /// and the same canonical per-eye plan FOVs (never reformatted or recalculated) go
        /// UNCHANGED — never Inversed here — to the retained runtime owner's RAW API
        /// CaptureRawPair(leftRaw, rightRaw, fovL, fovR, aspect 1.0, perspective true,
        /// ORIGINAL hosted Start generation, live stop callback); the legacy world CapturePair
        /// is never used for the runtime route. A Stop/generation invalidation is a pure
        /// CANCEL (OperationCanceledException, never recorded); a real owner rejection or a
        /// managed validation failure converges through RecordHostedCaptureFailure so the next
        /// tick can never continue SDK capture. The legacy default branch below is unchanged.</summary>
        private byte[][] CaptureStereoPair(Matrix rawLeft, double fovLeftDeg, Matrix rawRight, double fovRightDeg,
            out HostedPairTiming? hostedPairTiming)
        {
            hostedPairTiming = null;   // legacy route keeps null; only a validated hosted pair fills it
            if (_hostedBackendSelected)
            {
                int originalGen = _hostedStartGeneration;
                var ho = _hostedOwner;
                // An explicit Stop or generation invalidation is a CANCEL — never a recorded failure.
                if (!_running || _generation != originalGen)
                    throw new OperationCanceledException("VC_HOSTED_RAW_PAIR_CANCELED startGeneration=" + originalGen +
                        " running=" + (_running ? "true" : "false") + " generation=" + _generation);
                // Owner binding/admission recheck on a still-valid generation: any unexpected
                // state here is a REAL fault and converges through the managed failure cache gate.
                string pre = ho == null ? "hosted_raw_pair_owner_null"
                    : ho.BoundGeneration != originalGen ? "hosted_raw_pair_bound_generation_mismatch bound=" +
                        ho.BoundGeneration.ToString(CultureInfo.InvariantCulture)
                    : !string.IsNullOrEmpty(ho.FailureReason) ? "hosted_raw_pair_owner_failure " + ho.FailureReason
                    : !ho.AdmissionOpen ? "hosted_raw_pair_admission_closed generation=" + originalGen
                    : null;
                if (pre != null)
                {
                    RecordHostedCaptureFailure(originalGen, pre);
                    throw new InvalidOperationException("producer_stereo_reject reason=" + pre);
                }
                NC04.NC01CaptureSampleV1[] samples;
                try
                {
                    samples = ho.CaptureRawPair(rawLeft, rawRight, fovLeftDeg, fovRightDeg,
                        1.0, true, originalGen, () => !_running || _generation != originalGen);
                }
                catch (OperationCanceledException)
                {
                    throw;   // cancel: whole pair discarded, nothing recorded, no submit
                }
                catch (Exception ex)
                {
                    if (_running && _generation == originalGen)
                        RecordHostedCaptureFailure(originalGen,
                            "hosted_raw_pair_failed " + ex.GetType().Name + ": " + ex.Message);
                    throw;
                }
                string invalid = ValidateHostedRawPair(samples, fovLeftDeg, fovRightDeg);
                if (invalid != null)
                {
                    if (_running && _generation == originalGen) RecordHostedCaptureFailure(originalGen, invalid);
                    throw new InvalidOperationException("producer_stereo_reject reason=" + invalid);
                }
                // Profiling copy: only the EXISTING numeric fields of the already-validated samples are
                // read here; nothing is re-measured and no sample/array reference is retained.
                var hsL = samples[0];
                var hsR = samples[1];
                var ht = new HostedPairTiming
                {
                    OriginalGeneration = originalGen,
                    PairPrepareMs = hsL.PairPrepareMs,
                    PairFinalizeMs = hsL.PairFinalizeMs,
                    EyeCameraSumMs = hsL.EyeCameraMs + hsR.EyeCameraMs,
                    EyeLockWaitSumMs = hsL.EyeLockWaitMs + hsR.EyeLockWaitMs,
                    EyeSurfaceRenderSumMs = hsL.SurfaceRenderElapsedMs + hsR.SurfaceRenderElapsedMs,
                    EyeStageValidationSumMs = hsL.EyeStageValidationMs + hsR.EyeStageValidationMs,
                    EyeAllocateSumMs = hsL.EyeAllocateMs + hsR.EyeAllocateMs,
                    EyeReadbackCallSumMs = hsL.EyeReadbackCallMs + hsR.EyeReadbackCallMs,
                    EyeUnlockSumMs = hsL.EyeUnlockMs + hsR.EyeUnlockMs,
                    NativeReadbackSumMs = ((double)hsL.ReadbackQpcTicks + (double)hsR.ReadbackQpcTicks) * 1000.0 / Stopwatch.Frequency,
                    NativeCopySumMs = ((double)hsL.CopyQpcTicks + (double)hsR.CopyQpcTicks) * 1000.0 / Stopwatch.Frequency,
                    CapturePairMs = double.NaN   // stamped by the caller's SAME-tick capturePairMs at collection
                };
                hostedPairTiming = ht;
                // Fresh private per-call buffers, returned untouched — the caller's existing
                // post-capture running/generation gate still discards a stopped pair pre-submit.
                return new byte[][] { samples[0].Bgra, samples[1].Bgra };
            }
            var owner = _nativeOwner;
            if (owner == null || !owner.Begun)
                throw new InvalidOperationException("capture owner missing or not begun");
            return owner.CaptureStereo(rawLeft, fovLeftDeg, rawRight, fovRightDeg);
        }

        // Hosted raw-pair validation — MANAGED values/arrays only (no SDK object reads, no
        // reuse/mutation of the returned buffers). Per eye: identity, exact private payload
        // length, 64-byte packed surface info (structBytes=64/version=1/extent == the enforced
        // profile), a finite 16-entry actual view matrix and actual projection readback
        // (perspective, FOV/aspect within the owner's 1e-6 raw tolerance), finite gaze and a
        // stable owned stage snapshot; the pair must additionally carry one identical
        // non-empty before/after main snapshot. Returns null when everything passes,
        // otherwise one concrete rejection reason.
        private static string ValidateHostedRawPair(NC04.NC01CaptureSampleV1[] samples, double fovLeftDeg, double fovRightDeg)
        {
            if (samples == null || samples.Length != 2 || samples[0] == null || samples[1] == null)
                return "hosted_raw_pair_malformed_samples";
            if (samples[0].Bgra != null && ReferenceEquals(samples[0].Bgra, samples[1].Bgra))
                return "hosted_raw_pair_shared_payload";
            string e0 = ValidateHostedRawEye(samples[0], 0, fovLeftDeg);
            if (e0 != null) return e0;
            string e1 = ValidateHostedRawEye(samples[1], 1, fovRightDeg);
            if (e1 != null) return e1;
            string before = samples[0].PairMainBeforeSnapshot;
            if (string.IsNullOrEmpty(before) ||
                !string.Equals(before, samples[0].PairMainAfterSnapshot, StringComparison.Ordinal) ||
                !string.Equals(before, samples[1].PairMainBeforeSnapshot, StringComparison.Ordinal) ||
                !string.Equals(before, samples[1].PairMainAfterSnapshot, StringComparison.Ordinal))
                return "hosted_raw_pair_main_snapshot_mismatch";
            return null;
        }

        private static string ValidateHostedRawEye(NC04.NC01CaptureSampleV1 s, int eye, double fovDeg)
        {
            string tag = "eye" + eye.ToString(CultureInfo.InvariantCulture);
            if (s.Eye != eye)
                return "hosted_raw_pair_eye_identity " + tag + " actual=" + s.Eye.ToString(CultureInfo.InvariantCulture);
            if (s.Bgra == null || s.Bgra.Length != CaptureProfile.PayloadBytes)
                return "hosted_raw_pair_payload_length " + tag + " actual=" +
                    (s.Bgra == null ? "null" : s.Bgra.Length.ToString(CultureInfo.InvariantCulture));
            var info = s.SurfaceInfo;
            if (info == null || info.Length != 64) return "hosted_raw_pair_info_not64 " + tag;
            if (ReadU32Le(info, 0) != 64u || ReadU32Le(info, 4) != 1u)
                return "hosted_raw_pair_abi_mismatch " + tag +
                    " structBytes=" + ReadU32Le(info, 0).ToString(CultureInfo.InvariantCulture) +
                    " version=" + ReadU32Le(info, 4).ToString(CultureInfo.InvariantCulture);
            if (ReadU32Le(info, 8) != (uint)FrameW || ReadU32Le(info, 12) != (uint)FrameH)
                return "hosted_raw_pair_extent_mismatch " + tag +
                    " actual=" + ReadU32Le(info, 8).ToString(CultureInfo.InvariantCulture) +
                    "x" + ReadU32Le(info, 12).ToString(CultureInfo.InvariantCulture) +
                    " expected=" + FrameW.ToString(CultureInfo.InvariantCulture) +
                    "x" + FrameH.ToString(CultureInfo.InvariantCulture);
            if (s.ActualViewMatrix == null || s.ActualViewMatrix.Length != 16 || !AllFinite(s.ActualViewMatrix))
                return "hosted_raw_pair_actual_matrix_invalid " + tag;
            if (s.Ortho) return "hosted_raw_pair_ortho_mismatch " + tag;
            if (!Near(s.FovY, fovDeg))
                return "hosted_raw_pair_fov_mismatch " + tag +
                    " expected=" + fovDeg.ToString("R", CultureInfo.InvariantCulture) +
                    " actual=" + s.FovY.ToString("R", CultureInfo.InvariantCulture);
            if (!Near(s.AspectRatio, 1.0))
                return "hosted_raw_pair_aspect_mismatch " + tag +
                    " actual=" + s.AspectRatio.ToString("R", CultureInfo.InvariantCulture);
            if (double.IsNaN(s.Gaze) || double.IsInfinity(s.Gaze)) return "hosted_raw_pair_gaze_not_finite " + tag;
            if (string.IsNullOrEmpty(s.OwnedStageSnapshot)) return "hosted_raw_pair_stage_snapshot_missing " + tag;
            return null;
        }

        // NC01SurfaceInfoV1 (Pack=8): the first four uint32 fields at byte offsets 0/4/8/12, little-endian.
        private static uint ReadU32Le(byte[] b, int off)
        {
            return (uint)(b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24));
        }

        // ---------------- 02D per-eye asymmetric-FOV projection plan (reflection-testable) ----------------
        // Native XrCompositionLayerProjection declares the TRUE asymmetric per-eye fov
        // (fovLeft<0<fovRight, fovDown<0<fovUp, radians). The SIM renderer only knows a
        // symmetric FOVY, so the SOURCE frustum is widened to cover the full XR box
        // (ty = max(U, -D, max(-L, R)/aspect), tx = aspect*ty, aspect = FrameW/FrameH from
        // the SELECTED CaptureProfile), then the selected-size image is CPU-resampled so
        // every target pixel center carries the XR ray. CPU projection bootstrap — it does
        // NOT raise the selected capture-size limit (legacy was 640x480).
        internal struct EyeProjectionPlan
        {
            public bool Valid;
            public string RejectReason;
            public double FovLeftRad, FovRightRad, FovUpRad, FovDownRad;
            public double TanL, TanR, TanU, TanD;   // XR frustum tangents (L,D < 0 < R,U)
            public double Tx, Ty;                    // symmetric source half-tangents
            public double SourceFovYDeg;             // symmetric source camera FieldOfViewY
        }

        internal static EyeProjectionPlan BuildEyeProjectionPlan(
            double fovLeftRad, double fovRightRad, double fovUpRad, double fovDownRad)
        {
            var p = new EyeProjectionPlan { RejectReason = "" };
            if (double.IsNaN(fovLeftRad) || double.IsInfinity(fovLeftRad) ||
                double.IsNaN(fovRightRad) || double.IsInfinity(fovRightRad) ||
                double.IsNaN(fovUpRad) || double.IsInfinity(fovUpRad) ||
                double.IsNaN(fovDownRad) || double.IsInfinity(fovDownRad))
            { p.RejectReason = "non_finite"; return p; }
            if (!(fovLeftRad < 0 && fovRightRad > 0)) { p.RejectReason = "horiz_sign(left<0<right)"; return p; }
            if (!(fovDownRad < 0 && fovUpRad > 0)) { p.RejectReason = "vert_sign(down<0<up)"; return p; }
            const double halfPi = Math.PI / 2.0;
            if (fovLeftRad <= -halfPi || fovRightRad >= halfPi || fovUpRad >= halfPi || fovDownRad <= -halfPi)
            { p.RejectReason = "fov_range_ge_pi/2"; return p; }
            p.FovLeftRad = fovLeftRad; p.FovRightRad = fovRightRad;
            p.FovUpRad = fovUpRad; p.FovDownRad = fovDownRad;
            p.TanL = Math.Tan(fovLeftRad); p.TanR = Math.Tan(fovRightRad);
            p.TanU = Math.Tan(fovUpRad); p.TanD = Math.Tan(fovDownRad);
            double aspect = (double)FrameW / FrameH;
            p.Ty = Math.Max(p.TanU, Math.Max(-p.TanD, Math.Max(-p.TanL, p.TanR) / aspect));
            if (!(p.Ty > 0) || double.IsInfinity(p.Ty)) { p.RejectReason = "ty_invalid"; return p; }
            p.Tx = aspect * p.Ty;
            p.SourceFovYDeg = 2.0 * Math.Atan(p.Ty) * 180.0 / Math.PI;
            p.Valid = true;
            return p;
        }

        // Target pixel center -> XR ray tangent (x: L..R across columns, y: U..D down rows).
        internal static double XrRayXForTargetColumn(int x, int w, EyeProjectionPlan p)
        { return p.TanL + (p.TanR - p.TanL) * (x + 0.5) / w; }
        internal static double XrRayYForTargetRow(int y, int h, EyeProjectionPlan p)
        { return p.TanU + (p.TanD - p.TanU) * (y + 0.5) / h; }
        // XR ray tangent -> symmetric SOURCE image coordinate (pixel centers at .5 offsets).
        internal static double SourceUForRayX(double rayX, double tx, double w)
        { return (0.5 + rayX / (2.0 * tx)) * w - 0.5; }
        internal static double SourceVForRayY(double rayY, double ty, double h)
        { return (0.5 - rayY / (2.0 * ty)) * h - 0.5; }

        /// <summary>Bilinear BGRA resample of the symmetric source capture into the XR asymmetric
        /// frustum at the same selected-profile size. Edge-clamped, alpha forced 255.
        /// H2 perf: fixed-pointer inner loop (bounds-check elision) with the three color channels
        /// unrolled; identical double arithmetic order and Math.Round (ties-to-even) as baseline.</summary>
        internal static byte[] ResampleToXrFrustum(byte[] src, EyeProjectionPlan p)
        {
            if (src == null || src.Length != CaptureProfile.PayloadBytes)
                throw new ArgumentException("src must be " + FrameW + "x" + FrameH + " BGRA");
            if (!p.Valid) throw new ArgumentException("plan invalid: " + p.RejectReason);
            int W = FrameW, H = FrameH;
            var dst = new byte[W * H * 4];
            var fu = new double[W];
            var u0a = new int[W];
            for (int x = 0; x < W; x++)
            {
                double u = SourceUForRayX(XrRayXForTargetColumn(x, W, p), p.Tx, W);
                u = Math.Max(0.0, Math.Min(W - 1.0, u));   // clamp into valid pixel range (weights stay [0,1])
                int u0 = (int)Math.Floor(u);
                u0 = Math.Max(0, Math.Min(W - 2, u0));
                u0a[x] = u0; fu[x] = u - u0;
            }
            unsafe
            {
                fixed (byte* s = src)
                fixed (byte* d = dst)
                fixed (int* u0p = u0a)
                fixed (double* fup = fu)
                {
                    long stride = (long)W * 4;
                    for (int y = 0; y < H; y++)
                    {
                        double v = SourceVForRayY(XrRayYForTargetRow(y, H, p), p.Ty, H);
                        v = Math.Max(0.0, Math.Min(H - 1.0, v));
                        int v0 = (int)Math.Floor(v);
                        v0 = Math.Max(0, Math.Min(H - 2, v0));
                        double wv = v - v0;
                        byte* row0 = s + v0 * stride;          // source row v0
                        byte* row1 = row0 + stride;            // source row v0+1
                        byte* orow = d + y * stride;           // destination row y
                        for (int x = 0; x < W; x++)
                        {
                            int u0 = u0p[x];
                            double wu = fup[x];
                            byte* i00 = row0 + u0 * 4;
                            byte* i01 = i00 + 4;
                            byte* i10 = row1 + u0 * 4;
                            byte* i11 = i10 + 4;
                            byte* o = orow + x * 4;
                            // byte - byte promotes to signed int BEFORE * wu — identical to baseline
                            double topB = i00[0] + (i01[0] - i00[0]) * wu;
                            double botB = i10[0] + (i11[0] - i10[0]) * wu;
                            o[0] = (byte)Math.Round(topB + (botB - topB) * wv);
                            double topG = i00[1] + (i01[1] - i00[1]) * wu;
                            double botG = i10[1] + (i11[1] - i10[1]) * wu;
                            o[1] = (byte)Math.Round(topG + (botG - topG) * wv);
                            double topR = i00[2] + (i01[2] - i00[2]) * wu;
                            double botR = i10[2] + (i11[2] - i10[2]) * wu;
                            o[2] = (byte)Math.Round(topR + (botR - topR) * wv);
                            o[3] = 255;
                        }
                    }
                }
            }
            return dst;
        }

        /// <summary>Self-check harness — runnable by reflection WITHOUT a live session:
        /// invalid-input rejection, identity mapping, center/corners, full-frustum
        /// coverage, monotonicity.</summary>
        internal static string VerifyProjectionMappingAndPlan()
        {
            var sb = new StringBuilder();
            bool all = true;
            bool rejectInvalid =
                !BuildEyeProjectionPlan(double.NaN, 0.5, 0.5, -0.5).Valid &&
                !BuildEyeProjectionPlan(0.1, 0.5, 0.5, -0.5).Valid &&      // left not < 0
                !BuildEyeProjectionPlan(-0.5, 0.5, 0.5, 0.1).Valid &&      // down not < 0
                !BuildEyeProjectionPlan(-Math.PI / 2, 0.5, 0.5, -0.5).Valid && // |left| >= pi/2
                !BuildEyeProjectionPlan(-0.5, 0.5, Math.PI / 2, -0.5).Valid;   // up >= pi/2
            sb.Append("rejectInvalid=").Append(rejectInvalid ? "PASS" : "FAIL"); all &= rejectInvalid;

            // identity: a symmetric plan (TanL=-tx, TanR=tx, TanU=ty, TanD=-ty) must map
            // every target pixel center to itself — tx from the CURRENT profile aspect
            double ty0 = Math.Tan(0.5);
            double tx0 = ((double)FrameW / FrameH) * ty0;
            var id = BuildEyeProjectionPlan(-Math.Atan(tx0), Math.Atan(tx0), Math.Atan(ty0), -Math.Atan(ty0));
            bool identity = id.Valid;
            for (int x = 0; x < FrameW && identity; x++)
                for (int y = 0; y < FrameH && identity; y++)
                {
                    double u = SourceUForRayX(XrRayXForTargetColumn(x, FrameW, id), id.Tx, FrameW);
                    double v = SourceVForRayY(XrRayYForTargetRow(y, FrameH, id), id.Ty, FrameH);
                    if (Math.Abs(u - x) > 1e-9 || Math.Abs(v - y) > 1e-9) identity = false;
                }
            sb.Append(" identity=").Append(identity ? "PASS" : "FAIL"); all &= identity;

            // center ray -> exact source center pixel coordinate (319.5 / 239.5)
            bool center = Math.Abs(SourceUForRayX(0, tx0, FrameW) - (FrameW / 2.0 - 0.5)) < 1e-9
                && Math.Abs(SourceVForRayY(0, ty0, FrameH) - (FrameH / 2.0 - 0.5)) < 1e-9;
            sb.Append(" centerMapping=").Append(center ? "PASS" : "FAIL"); all &= center;

            // asymmetric XR corner rays stay within the source canvas (edge centers included)
            var asym = BuildEyeProjectionPlan(-0.9, 0.6, 0.8, -0.55);
            bool corners = asym.Valid;
            if (corners)
            {
                double[][] c4 = { new[] { asym.TanL, asym.TanU }, new[] { asym.TanR, asym.TanU },
                    new[] { asym.TanL, asym.TanD }, new[] { asym.TanR, asym.TanD } };
                foreach (var c in c4)
                {
                    double u = SourceUForRayX(c[0], asym.Tx, FrameW), v = SourceVForRayY(c[1], asym.Ty, FrameH);
                    if (u < -0.5 - 1e-9 || u > FrameW - 0.5 + 1e-9 ||
                        v < -0.5 - 1e-9 || v > FrameH - 0.5 + 1e-9) corners = false;
                }
            }
            sb.Append(" cornersInsideSource=").Append(corners ? "PASS" : "FAIL"); all &= corners;

            // the symmetric source box must contain the full asymmetric XR box on both axes
            bool cover = asym.Valid &&
                asym.Tx >= Math.Max(-asym.TanL, asym.TanR) - 1e-12 &&
                asym.Ty >= Math.Max(asym.TanU, -asym.TanD) - 1e-12;
            sb.Append(" frustumCoverage=").Append(cover ? "PASS" : "FAIL"); all &= cover;

            // monotonic: rayX grows with column; source v grows with row (top->bottom)
            bool mono = asym.Valid;
            for (int x = 1; x < FrameW && mono; x++)
                if (!(XrRayXForTargetColumn(x, FrameW, asym) > XrRayXForTargetColumn(x - 1, FrameW, asym))) mono = false;
            for (int y = 1; y < FrameH && mono; y++)
                if (!(SourceVForRayY(XrRayYForTargetRow(y, FrameH, asym), asym.Ty, FrameH) >
                      SourceVForRayY(XrRayYForTargetRow(y - 1, FrameH, asym), asym.Ty, FrameH))) mono = false;
            sb.Append(" monotonic=").Append(mono ? "PASS" : "FAIL"); all &= mono;

            sb.Append(" ALL=").Append(all ? "PASS" : "FAIL");
            return sb.ToString();
        }

        private static string PlanJson(string tag, EyeProjectionPlan p)
        {
            var ci = CultureInfo.InvariantCulture;
            return "\"" + tag + "\":{\"fovRad\":{\"left\":" + p.FovLeftRad.ToString("0.000000", ci) +
                ",\"right\":" + p.FovRightRad.ToString("0.000000", ci) +
                ",\"up\":" + p.FovUpRad.ToString("0.000000", ci) +
                ",\"down\":" + p.FovDownRad.ToString("0.000000", ci) + "}" +
                ",\"tan\":{\"L\":" + p.TanL.ToString("0.000000", ci) +
                ",\"R\":" + p.TanR.ToString("0.000000", ci) +
                ",\"U\":" + p.TanU.ToString("0.000000", ci) +
                ",\"D\":" + p.TanD.ToString("0.000000", ci) + "}" +
                ",\"tx\":" + p.Tx.ToString("0.000000", ci) +
                ",\"ty\":" + p.Ty.ToString("0.000000", ci) +
                ",\"sourceFovYDeg\":" + p.SourceFovYDeg.ToString("0.000", ci) + "}";
        }

        private void WriteProjectionEvidenceOnce(EyeProjectionPlan pl, EyeProjectionPlan pr)
        {
            if (_projectionEvidenceWritten) return;
            var owner = _nativeOwner;
            ulong captureCount = owner != null ? owner.CaptureCount : 0UL;
            bool actualNativeSizeVerified = owner != null && owner.Begun && captureCount > 0 &&
                owner.LastActualWidth == FrameW && owner.LastActualHeight == FrameH &&
                owner.LastPayloadBytes == CaptureProfile.PayloadBytes;
            _projectionEvidenceWritten = true;
            string check = VerifyProjectionMappingAndPlan();
            string selectedSize = FrameW + "x" + FrameH;
            string sizeNote = actualNativeSizeVerified
                ? "ACTUAL native readback width/height/payload VERIFIED to match the SELECTED " + selectedSize +
                  " profile (owner captureCount=" + captureCount + "); this verifies native readbacks ONLY — head tracking and stereo pose-image binding are evaluated separately"
                : "SELECTED vs ACTUAL not reconciled (owner not begun or no captures yet); head tracking and stereo pose-image binding are evaluated separately";
            string j = "{\"projectionMode\":\"LOWRES_CPU_ASYMMETRIC_REPROJECTION\"" +
                ",\"frame\":\"" + selectedSize + "\"" +
                ",\"selectedProfile\":\"" + CaptureProfile.Selected + "\"" +
                ",\"selectedSize\":\"" + selectedSize + " (requested/selected via CaptureProfile)\"" +
                ",\"actualNativeSizeVerified\":" + (actualNativeSizeVerified ? "true" : "false") +
                ",\"actualSizeNote\":\"" + sizeNote + "\"" +
                ",\"resolutionLimitUnchanged\":" + (CaptureProfile.Selected == "legacy-640x480" ? "true" : "false") +
                ",\"resolutionSelectionNote\":\"resolutionLimitUnchanged compares the SELECTED CaptureProfile configuration only; ACTUAL rendered extent comes from native owner readbacks (actualNativeSizeVerified)\"" +
                ",\"note\":\"symmetric SIM capture covers the FULL per-eye asymmetric XR frustum, then CPU bilinear resample aligns image rays with the headset XrCompositionLayerProjection fov; low-resolution CPU projection bootstrap - NOT a resolution upgrade beyond the SELECTED " + selectedSize + " profile size; pre-calibration entry frames make no ray-match claim\"" +
                ",\"selfCheck\":\"" + check.Replace("\r", " ").Replace("\n", " | ") + "\"" +
                ",\"plans\":{" + PlanJson("left", pl) + "," + PlanJson("right", pr) + "}" +
                ",\"capturedUtc\":\"" + DateTime.UtcNow.ToString("o") + "\"}";
            try { File.WriteAllText(Path.Combine(_evidenceDir, "projection-mapping.json"), j); } catch { }
            _host.Log("PROJECTION_EVIDENCE written selfCheck=" +
                (check.Contains("ALL=PASS") ? "ALL=PASS" : "SEE_JSON"));
        }

        // ---------------- quaternion / matrix helpers ----------------
        private static bool IsFinite(VRSIMPlugin.VRSIMEyeView v)
        {
            foreach (double d in v.posMm) if (double.IsNaN(d) || double.IsInfinity(d)) return false;
            foreach (double d in v.quatXyzw) if (double.IsNaN(d) || double.IsInfinity(d)) return false;
            return true;
        }
        private static bool ValidQ(double[] q)
        {
            double n = Math.Sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            return n > 0.9 && n < 1.1;
        }
        private static double[] NormalizeQ(double[] q)
        {
            double n = Math.Sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (n < 1e-9) return new[] { 0.0, 0.0, 0.0, 1.0 };
            return new[] { q[0] / n, q[1] / n, q[2] / n, q[3] / n };
        }
        /// <summary>Rotation matrix (row-major 9) from SIM-frame quaternion (x,y,z,w).</summary>
        /// <summary>02A PHASE 15: capture alpha is unreliable (A=0) — force opaque so PNG
        /// evidence is visible and hashes cover RGB meaningfully. HMD blend is OPAQUE anyway.</summary>
        internal static void ForceAlpha(byte[] bgra)
        {
            for (int i = 3; i < bgra.Length; i += 4) bgra[i] = 255;
        }

        internal static double[] QToMatrix(double[] q)
        {
            double x = q[0], y = q[1], z = q[2], w = q[3];
            return new[]
            {
                1 - 2*(y*y + z*z),     2*(x*y - z*w),         2*(x*z + y*w),
                2*(x*y + z*w),         1 - 2*(x*x + z*z),     2*(y*z - x*w),
                2*(x*z - y*w),         2*(y*z + x*w),         1 - 2*(x*x + y*y)
            };
        }
        internal static double[] Mul3(double[] a, double[] b)
        {
            var r = new double[9];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
            return r;
        }
        internal static double[] Trans3(double[] a)
        {
            return new[] { a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8] };
        }
        internal static double[] Apply3(double[] m, double[] v)
        {
            return new[]
            {
                m[0]*v[0] + m[1]*v[1] + m[2]*v[2],
                m[3]*v[0] + m[4]*v[1] + m[5]*v[2],
                m[6]*v[0] + m[7]*v[1] + m[8]*v[2]
            };
        }
        internal static double[] Add(double[] a, double[] b) { return new[] { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
        internal static double[] Sub(double[] a, double[] b) { return new[] { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
        private static double Sq(double v) { return v * v; }
        private static bool AllFinite(double[] v) { foreach (double d in v) if (double.IsNaN(d) || double.IsInfinity(d)) return false; return true; }

        /// <summary>Boundary setter: assembles the CAMERA-TO-WORLD pose (columns N/O/A = camera
        /// axes in world, X right / Y up / Z back; P = camera world position mm) and returns its
        /// SDK inverse — cam.ViewMatrix is WORLD-TO-CAMERA (pixel-verified), so the result is
        /// directly assignable to ViewMatrix.</summary>
        internal static Matrix RotPosToMat(double[] R, double[] p)
        {
            var m = new Matrix();
            m.Nx = R[0]; m.Ny = R[3]; m.Nz = R[6];   // column 0 = camera X (right)
            m.Ox = R[1]; m.Oy = R[4]; m.Oz = R[7];   // column 1 = camera Y (up)
            m.Ax = R[2]; m.Ay = R[5]; m.Az = R[8];   // column 2 = camera Z (back)
            m.Px = p[0]; m.Py = p[1]; m.Pz = p[2];
            return m.Inverse();
        }
        /// <summary>Boundary getter: input is the RAW ViewMatrix (world-to-camera). Inverted
        /// first, then extracts CAMERA-TO-WORLD R (row-major 9) and P (world position mm).</summary>
        internal static void MatToRotPos(Matrix m, double[] R, double[] p)
        {
            m = m.Inverse();   // world-to-camera → camera-to-world
            R[0] = m.Nx; R[1] = m.Ox; R[2] = m.Ax;   // row-major from columns
            R[3] = m.Ny; R[4] = m.Oy; R[5] = m.Ay;
            R[6] = m.Nz; R[7] = m.Oz; R[8] = m.Az;
            p[0] = m.Px; p[1] = m.Py; p[2] = m.Pz;
        }
        private static bool Near(double a, double b) { return Math.Abs(a - b) < 1e-6; }
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
        private static double ChangeRatio(byte[] a, byte[] b)
        {
            if (a == null || b == null || a.Length != b.Length) return 1.0;
            long changed = 0;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) changed++;
            return (double)changed / a.Length;
        }
        private void SavePng(string name, byte[] bgra)
        {
            try
            {
                var bs = System.Windows.Media.Imaging.BitmapSource.Create(FrameW, FrameH, 96, 96,
                    System.Windows.Media.PixelFormats.Bgra32, null, bgra, FrameW * 4);
                var enc = new System.Windows.Media.Imaging.PngBitmapEncoder();
                enc.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bs));
                using (var fs = File.Create(Path.Combine(_evidenceDir, name))) enc.Save(fs);
            }
            catch (Exception ex) { _host.Log("PNG_FAIL " + name + " " + ex.Message); }
        }
        private void WriteJson(string name, string content)
        {
            try { File.WriteAllText(Path.Combine(_evidenceDir, name), content); } catch { }
        }
    }
}
