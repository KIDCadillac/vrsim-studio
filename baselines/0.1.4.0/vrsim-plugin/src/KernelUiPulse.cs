using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;
using System.Windows.Threading;

namespace VRSIM
{
    internal sealed class KernelUiPulse
    {
        private const uint CreateWaitableTimerHighResolution = 0x00000002;
        private const uint TimerAccess = 0x00100002;
        private const long DueRelative1MsIn100ns = -10000L;
        private const int Period1Ms = 1;
        private const uint TolerableDelay = 0;
        private const uint Infinite = 0xFFFFFFFF;
        private const uint WaitIndexStop = 0;
        private const uint WaitIndexTick = 1;
        private const int StopBudgetMs = 1000;
        private const int RenderPacedMaximumUiStartHz = 120;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeWaitableTimerHandle CreateWaitableTimerExW(
            IntPtr lpTimerAttributes,
            string lpTimerName,
            uint dwFlags,
            uint dwAccess);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetWaitableTimerEx(
            SafeWaitableTimerHandle hTimer,
            ref long lpDueTime,
            int lPeriod,
            IntPtr pfnCompletionRoutine,
            IntPtr lpArgToCompletionRoutine,
            IntPtr WakeContext,
            uint TolerableDelay);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CancelWaitableTimer(SafeWaitableTimerHandle hTimer);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForMultipleObjects(
            uint nCount,
            IntPtr[] lpHandles,
            [MarshalAs(UnmanagedType.Bool)] bool bWaitAll,
            uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr hObject);

        private sealed class SafeWaitableTimerHandle : SafeHandleZeroOrMinusOneIsInvalid
        {
            internal SafeWaitableTimerHandle()
                : base(true)
            {
            }

            protected override bool ReleaseHandle()
            {
                return CloseHandle(handle);
            }
        }

        private sealed class PulseStopEvent : EventWaitHandle
        {
            internal PulseStopEvent()
                : base(false, EventResetMode.ManualReset)
            {
            }

            internal SafeWaitHandle OwnSafeWaitHandle
            {
                get { return SafeWaitHandle; }
            }
        }

        private readonly Dispatcher _dispatcher;
        private readonly Action _onUiPulse;
        private readonly object _gate = new object();

        private SafeWaitableTimerHandle _timerHandle;
        private PulseStopEvent _stopEvent;
        private Thread _worker;
        private Task<KernelUiPulseStopResult> _stopTask;

        private bool _startAttempted;
        private bool _started;
        private bool _stopRequested;
        private bool _workerExited;
        private bool _handlesReleased = true;

        private long _pendingToken;
        private long _nextToken;
        private DispatcherOperation _pendingOperation;
        private bool _callbackExecuting;

        // Frozen per-instance callback mode: the legacy two-argument constructor is ALWAYS
        // Background without pacing; the internal overload with renderPacedMode=true freezes
        // Render priority plus the bounded 120 UI-action starts/second QPC pacing. Priority and
        // pacing are never switched after construction and never read environment/SDK state.
        private readonly bool _renderPaced;
        private readonly DispatcherPriority _callbackPriority;
        private readonly long _minimumUiStartSpacingQpc;
        private long _nextUiStartQpc;         // worker posts ONLY at/after this QPC deadline
        private long _lastUiStartQpc;         // ACTUAL admitted UI-action start stamp (0 = never)
        private long _pacingSkippedSignals;   // distinct from pending/executing coalesced signals

        private long _timerSignals;
        private long _coalescedSignals;
        private long _postedCallbacks;
        private long _executedCallbacks;
        private long _lastWin32Error;
        private string _failure;

        internal KernelUiPulse(Dispatcher dispatcher, Action onUiPulse)
            : this(dispatcher, onUiPulse, false)
        {
        }

        internal KernelUiPulse(Dispatcher dispatcher, Action onUiPulse, bool renderPacedMode)
        {
            if (dispatcher == null)
            {
                throw new ArgumentNullException("dispatcher");
            }
            if (onUiPulse == null)
            {
                throw new ArgumentNullException("onUiPulse");
            }
            if (!dispatcher.CheckAccess())
            {
                throw new InvalidOperationException("KernelUiPulse must be constructed on its dispatcher thread.");
            }
            _dispatcher = dispatcher;
            _onUiPulse = onUiPulse;
            _renderPaced = renderPacedMode;
            _callbackPriority = renderPacedMode ? DispatcherPriority.Render : DispatcherPriority.Background;
            _minimumUiStartSpacingQpc = renderPacedMode
                ? (long)Math.Ceiling((double)Stopwatch.Frequency / RenderPacedMaximumUiStartHz)
                : 0L;
        }

        internal bool TryStart(out string reason)
        {
            reason = null;
            if (!_dispatcher.CheckAccess())
            {
                reason = "TryStart must be called on the dispatcher thread.";
                return false;
            }

            lock (_gate)
            {
                if (_startAttempted || _stopTask != null)
                {
                    reason = "KernelUiPulse is one-shot; restart is not supported.";
                    return false;
                }
                _startAttempted = true;
            }

            SafeWaitableTimerHandle timer = CreateWaitableTimerExW(
                IntPtr.Zero, null, CreateWaitableTimerHighResolution, TimerAccess);
            if (timer == null || timer.IsInvalid || timer.IsClosed)
            {
                int error = Marshal.GetLastWin32Error();
                bool timerReleased = (timer == null);
                if (timer != null)
                {
                    try
                    {
                        timer.Dispose();
                        timerReleased = true;
                    }
                    catch (Exception)
                    {
                    }
                }
                reason = "CreateWaitableTimerExW failed (Win32 " + error + "); no low-precision fallback is used.";
                lock (_gate)
                {
                    _lastWin32Error = error;
                    if (_failure == null)
                    {
                        _failure = reason;
                    }
                    if (!timerReleased)
                    {
                        // Dispose failed: keep the reference and never claim handles released.
                        _timerHandle = timer;
                        _handlesReleased = false;
                    }
                }
                return false;
            }

            PulseStopEvent stopEvent = null;
            Thread worker = null;
            try
            {
                stopEvent = new PulseStopEvent();
                long due = DueRelative1MsIn100ns;
                bool armed = SetWaitableTimerEx(
                    timer, ref due, Period1Ms, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, TolerableDelay);
                if (!armed)
                {
                    int error = Marshal.GetLastWin32Error();
                    reason = "SetWaitableTimerEx failed (Win32 " + error + ").";

                    // The worker never received these handles, so release both here, each independently.
                    bool timerReleased = false;
                    bool stopEventReleased = (stopEvent == null);
                    try
                    {
                        timer.Dispose();
                        timerReleased = true;
                    }
                    catch (Exception)
                    {
                    }
                    if (stopEvent != null)
                    {
                        try
                        {
                            stopEvent.Dispose();
                            stopEventReleased = true;
                        }
                        catch (Exception)
                        {
                        }
                    }

                    lock (_gate)
                    {
                        _lastWin32Error = error;
                        if (_failure == null)
                        {
                            _failure = reason;
                        }
                        _timerHandle = timerReleased ? (SafeWaitableTimerHandle)null : timer;
                        _stopEvent = stopEventReleased ? null : stopEvent;
                        _handlesReleased = timerReleased && stopEventReleased;
                    }
                    return false;
                }

                worker = new Thread(new ThreadStart(WorkerLoop));
                worker.IsBackground = true;
                worker.Name = "VRSIM.KernelUiPulse";

                // Fields are published before Start; the worker must see live handles on its first wait.
                lock (_gate)
                {
                    _timerHandle = timer;
                    _stopEvent = stopEvent;
                    _worker = worker;
                    _handlesReleased = false;
                    _started = true;
                }

                try
                {
                    worker.Start();
                }
                catch
                {
                    lock (_gate)
                    {
                        _started = false;
                        _worker = null;
                    }
                    throw;
                }
                return true;
            }
            catch (Exception ex)
            {
                // The worker never entered a wait, so pre-start cleanup may release handles directly.
                string failure = "KernelUiPulse start failed: " + ex.GetType().Name + ": " + ex.Message;
                if (_started)
                {
                    try { CancelWaitableTimer(timer); }
                    catch (Exception) { }
                }
                bool timerReleased = false;
                bool stopEventReleased = (stopEvent == null);
                try
                {
                    timer.Dispose();
                    timerReleased = true;
                }
                catch (Exception)
                {
                }
                if (stopEvent != null)
                {
                    try
                    {
                        stopEvent.Dispose();
                        stopEventReleased = true;
                    }
                    catch (Exception)
                    {
                    }
                }
                lock (_gate)
                {
                    _started = false;
                    _worker = null;
                    _timerHandle = timerReleased ? (SafeWaitableTimerHandle)null : timer;
                    _stopEvent = stopEventReleased ? null : stopEvent;
                    _handlesReleased = timerReleased && stopEventReleased;
                    if (_failure == null)
                    {
                        _failure = failure;
                    }
                }
                reason = failure;
                return false;
            }
        }

        internal Task<KernelUiPulseStopResult> StopAsync()
        {
            lock (_gate)
            {
                _stopRequested = true;
                if (_stopEvent != null)
                {
                    try
                    {
                        _stopEvent.Set();
                    }
                    catch (ObjectDisposedException)
                    {
                    }
                }
                if (_stopTask == null)
                {
                    _stopTask = Task.Run(new Func<KernelUiPulseStopResult>(CompleteStop));
                }
                return _stopTask;
            }
        }

        internal KernelUiPulseSnapshot ReadSnapshot()
        {
            lock (_gate)
            {
                return new KernelUiPulseSnapshot(
                    _started,
                    _stopRequested,
                    _workerExited,
                    _pendingToken != 0 && !_callbackExecuting,
                    _callbackExecuting,
                    _timerSignals,
                    _coalescedSignals,
                    _postedCallbacks,
                    _executedCallbacks,
                    _lastWin32Error,
                    _failure,
                    _callbackPriority.ToString(),
                    _renderPaced ? RenderPacedMaximumUiStartHz : 0,
                    _pacingSkippedSignals,
                    _lastUiStartQpc);
            }
        }

        private void WorkerLoop()
        {
            try
            {
                while (true)
                {
                    uint waitResult = WaitForSignal();
                    if (waitResult == WaitIndexStop)
                    {
                        break;
                    }
                    if (waitResult == WaitIndexTick)
                    {
                        SignalTick();
                        continue;
                    }
                    break;
                }
            }
            catch (Exception ex)
            {
                PostFailure("KernelUiPulse worker fault: " + ex.GetType().Name + ": " + ex.Message, 0);
            }
            finally
            {
                lock (_gate)
                {
                    _workerExited = true;
                    Monitor.PulseAll(_gate);
                }
            }
        }

        private uint WaitForSignal()
        {
            SafeWaitableTimerHandle timer = _timerHandle;
            PulseStopEvent stopEvent = _stopEvent;
            if (timer == null || stopEvent == null)
            {
                PostFailure("KernelUiPulse worker found no live wait handles.", 0);
                return uint.MaxValue;
            }

            bool timerAddRef = false;
            bool stopAddRef = false;
            try
            {
                // Both handles are pinned across the wait so they cannot be released mid-wait.
                timer.DangerousAddRef(ref timerAddRef);
                stopEvent.OwnSafeWaitHandle.DangerousAddRef(ref stopAddRef);
                IntPtr[] handles = new IntPtr[2]
                {
                    stopEvent.OwnSafeWaitHandle.DangerousGetHandle(),
                    timer.DangerousGetHandle()
                };
                uint result = WaitForMultipleObjects(2, handles, false, Infinite);
                if (result == WaitIndexStop || result == WaitIndexTick)
                {
                    return result;
                }
                int error = Marshal.GetLastWin32Error();
                PostFailure("WaitForMultipleObjects failed (Win32 " + error + ").", error);
                return uint.MaxValue;
            }
            catch (Exception ex)
            {
                PostFailure("KernelUiPulse wait failed: " + ex.GetType().Name + ": " + ex.Message, 0);
                return uint.MaxValue;
            }
            finally
            {
                if (stopAddRef)
                {
                    stopEvent.OwnSafeWaitHandle.DangerousRelease();
                }
                if (timerAddRef)
                {
                    timer.DangerousRelease();
                }
            }
        }

        private void SignalTick()
        {
            lock (_gate)
            {
                if (!_started || _stopRequested || _failure != null)
                {
                    return;
                }
                _timerSignals++;
                if (_pendingToken != 0)
                {
                    _coalescedSignals++;
                    return;
                }
                if (_renderPaced && Stopwatch.GetTimestamp() < _nextUiStartQpc)
                {
                    // Pacing-skipped is a scheduler fact DISTINCT from a coalesced signal; the
                    // deadline advances only at an ACTUAL admitted UI action start, so a callback
                    // delayed in the dispatcher queue can never trigger an immediate catch-up burst.
                    _pacingSkippedSignals++;
                    return;
                }

                _nextToken++;
                long token = _nextToken;
                _pendingToken = token;

                DispatcherOperation op;
                try
                {
                    op = _dispatcher.BeginInvoke(_callbackPriority, new Action(() => UiCallback(token)));
                }
                catch (Exception ex)
                {
                    _pendingToken = 0;
                    PostFailure("KernelUiPulse BeginInvoke failed: " + ex.GetType().Name + ": " + ex.Message, 0);
                    return;
                }
                _pendingOperation = op;
                _postedCallbacks++;
            }
        }

        private void UiCallback(long token)
        {
            bool runAction = false;
            lock (_gate)
            {
                if (_pendingToken == token)
                {
                    if (!_stopRequested && _failure == null && !_callbackExecuting)
                    {
                        _callbackExecuting = true;
                        _executedCallbacks++;
                        // The pacing deadline advances ONLY at the ACTUAL admitted UI action START
                        // under this same gate — never at worker-post time and never at action end.
                        _lastUiStartQpc = Stopwatch.GetTimestamp();
                        if (_renderPaced)
                        {
                            _nextUiStartQpc = _lastUiStartQpc + _minimumUiStartSpacingQpc;
                        }
                        runAction = true;
                    }
                    else
                    {
                        // Dead-callback gate: admission closed, so this lease releases its own token.
                        _pendingToken = 0;
                        _pendingOperation = null;
                        Monitor.PulseAll(_gate);
                    }
                }
            }

            if (!runAction)
            {
                return;
            }

            try
            {
                _onUiPulse();
            }
            catch (Exception ex)
            {
                PostFailure("KernelUiPulse UI action fault: " + ex.GetType().Name + ": " + ex.Message, 0);
            }
            finally
            {
                lock (_gate)
                {
                    _callbackExecuting = false;
                    if (_pendingToken == token)
                    {
                        _pendingToken = 0;
                        _pendingOperation = null;
                    }
                    Monitor.PulseAll(_gate);
                }
            }
        }

        private void PostFailure(string message, int win32Error)
        {
            PulseStopEvent stopEvent;
            lock (_gate)
            {
                if (_failure == null)
                {
                    _failure = message;
                }
                if (win32Error != 0)
                {
                    _lastWin32Error = win32Error;
                }
                _stopRequested = true;
                stopEvent = _stopEvent;
            }
            if (stopEvent != null)
            {
                try
                {
                    stopEvent.Set();
                }
                catch (ObjectDisposedException)
                {
                }
            }
        }

        private KernelUiPulseStopResult CompleteStop()
        {
            int deadline = Environment.TickCount + StopBudgetMs;

            Thread worker = _worker;
            bool workerExited;
            if (worker == null)
            {
                workerExited = true;
            }
            else
            {
                int remaining = DeadlineRemaining(deadline);
                workerExited = remaining > 0 && worker.Join(remaining);
                if (!workerExited)
                {
                    // Join budget exhausted: keep every handle and lease reference; never claim success.
                    return BuildStopResult(false, false, false, true);
                }
            }

            bool drained = false;
            bool timedOut = false;
            while (true)
            {
                lock (_gate)
                {
                    if (_callbackExecuting)
                    {
                        // Action still runs; wait for its finally to release the lease.
                    }
                    else if (_pendingToken == 0)
                    {
                        drained = true;
                    }
                    else
                    {
                        DispatcherOperation op = _pendingOperation;
                        if (op != null)
                        {
                            DispatcherOperationStatus status = op.Status;
                            if (status == DispatcherOperationStatus.Pending)
                            {
                                op.Abort();
                                status = op.Status;
                            }
                            if (status == DispatcherOperationStatus.Aborted)
                            {
                                _pendingToken = 0;
                                _pendingOperation = null;
                                drained = true;
                            }
                        }
                    }

                    if (drained)
                    {
                        break;
                    }
                    int remaining = DeadlineRemaining(deadline);
                    if (remaining <= 0)
                    {
                        timedOut = true;
                        break;
                    }
                    Monitor.Wait(_gate, remaining);
                }
            }

            bool handlesReleased;
            if (drained)
            {
                handlesReleased = ReleaseKernelResources();
            }
            else
            {
                lock (_gate)
                {
                    handlesReleased = _handlesReleased;
                }
            }
            return BuildStopResult(workerExited, drained, handlesReleased, timedOut);
        }

        private bool ReleaseKernelResources()
        {
            SafeWaitableTimerHandle timer;
            PulseStopEvent stopEvent;
            lock (_gate)
            {
                // Fields keep their references until each object is actually released.
                timer = _timerHandle;
                stopEvent = _stopEvent;
            }

            bool timerReleased = (timer == null);
            bool stopEventReleased = (stopEvent == null);

            if (timer != null)
            {
                try
                {
                    if (!CancelWaitableTimer(timer))
                    {
                        int error = Marshal.GetLastWin32Error();
                        lock (_gate)
                        {
                            _lastWin32Error = error;
                            if (_failure == null)
                            {
                                _failure = "CancelWaitableTimer failed (Win32 " + error + "); worker already exited, handles are still released.";
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    lock (_gate)
                    {
                        if (_failure == null)
                        {
                            _failure = "CancelWaitableTimer threw: " + ex.GetType().Name + ": " + ex.Message;
                        }
                    }
                }
            }

            if (timer != null)
            {
                try
                {
                    timer.Dispose();
                    timerReleased = true;
                }
                catch (Exception ex)
                {
                    lock (_gate)
                    {
                        if (_failure == null)
                        {
                            _failure = "Timer handle Dispose failed: " + ex.GetType().Name + ": " + ex.Message;
                        }
                    }
                }
            }

            if (stopEvent != null)
            {
                try
                {
                    stopEvent.Set();
                }
                catch (Exception ex)
                {
                    lock (_gate)
                    {
                        if (_failure == null)
                        {
                            _failure = "Stop event Set failed: " + ex.GetType().Name + ": " + ex.Message;
                        }
                    }
                }
                try
                {
                    stopEvent.Dispose();
                    stopEventReleased = true;
                }
                catch (Exception ex)
                {
                    lock (_gate)
                    {
                        if (_failure == null)
                        {
                            _failure = "Stop event Dispose failed: " + ex.GetType().Name + ": " + ex.Message;
                        }
                    }
                }
            }

            lock (_gate)
            {
                if (timerReleased)
                {
                    _timerHandle = null;
                }
                if (stopEventReleased)
                {
                    _stopEvent = null;
                }
                _handlesReleased = timerReleased && stopEventReleased;
                return _handlesReleased;
            }
        }

        private KernelUiPulseStopResult BuildStopResult(bool workerExited, bool drained, bool handlesReleased, bool timedOut)
        {
            lock (_gate)
            {
                bool pending = _pendingToken != 0 && !_callbackExecuting;
                bool executing = _callbackExecuting;
                bool stopVerified = workerExited && drained && handlesReleased && !timedOut && !pending && !executing;
                return new KernelUiPulseStopResult(
                    stopVerified,
                    workerExited,
                    pending,
                    executing,
                    handlesReleased,
                    timedOut,
                    _failure);
            }
        }

        private static int DeadlineRemaining(int deadline)
        {
            int remaining = deadline - Environment.TickCount;
            return remaining < 0 ? 0 : remaining;
        }
    }

    internal sealed class KernelUiPulseStopResult
    {
        internal readonly bool StopVerified;
        internal readonly bool WorkerExited;
        internal readonly bool CallbackPending;
        internal readonly bool CallbackExecuting;
        internal readonly bool HandlesReleased;
        internal readonly bool TimedOut;
        internal readonly string Failure;

        internal KernelUiPulseStopResult(
            bool stopVerified,
            bool workerExited,
            bool callbackPending,
            bool callbackExecuting,
            bool handlesReleased,
            bool timedOut,
            string failure)
        {
            StopVerified = stopVerified;
            WorkerExited = workerExited;
            CallbackPending = callbackPending;
            CallbackExecuting = callbackExecuting;
            HandlesReleased = handlesReleased;
            TimedOut = timedOut;
            Failure = failure;
        }
    }

    internal sealed class KernelUiPulseSnapshot
    {
        internal readonly bool Started;
        internal readonly bool StopRequested;
        internal readonly bool WorkerExited;
        internal readonly bool CallbackPending;
        internal readonly bool CallbackExecuting;
        internal readonly long TimerSignals;
        internal readonly long CoalescedSignals;
        internal readonly long PostedCallbacks;
        internal readonly long ExecutedCallbacks;
        internal readonly long LastWin32Error;
        internal readonly string Failure;
        // Frozen scheduler facts — NOT FPS: the per-instance callback priority and pacing cap
        // chosen at construction, the pacing-skipped signal count (distinct from coalesced
        // pending/executing signals) and the last ACTUAL admitted UI-action start QPC stamp.
        internal readonly string CallbackPriority;
        internal readonly int MaximumUiStartHz;
        internal readonly long PacingSkippedSignals;
        internal readonly long LastUiStartQpc;

        internal KernelUiPulseSnapshot(
            bool started,
            bool stopRequested,
            bool workerExited,
            bool callbackPending,
            bool callbackExecuting,
            long timerSignals,
            long coalescedSignals,
            long postedCallbacks,
            long executedCallbacks,
            long lastWin32Error,
            string failure,
            string callbackPriority,
            int maximumUiStartHz,
            long pacingSkippedSignals,
            long lastUiStartQpc)
        {
            Started = started;
            StopRequested = stopRequested;
            WorkerExited = workerExited;
            CallbackPending = callbackPending;
            CallbackExecuting = callbackExecuting;
            TimerSignals = timerSignals;
            CoalescedSignals = coalescedSignals;
            PostedCallbacks = postedCallbacks;
            ExecutedCallbacks = executedCallbacks;
            LastWin32Error = lastWin32Error;
            Failure = failure;
            CallbackPriority = callbackPriority;
            MaximumUiStartHz = maximumUiStartHz;
            PacingSkippedSignals = pacingSkippedSignals;
            LastUiStartQpc = lastUiStartQpc;
        }
    }
}
