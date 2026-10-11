// VRMonitorWindow.cs — 0.1.3.5 READ-ONLY live monitor for the VRSIM pipeline.
//
// Shows the LAST SUCCESSFULLY SUBMITTED stereo pair (cached at the single SubmitStereo
// entry point in VRSIMPlugin) plus counters read ONLY through the host's existing
// native ABI wrappers. It NEVER: owns an XR session, touches the VC camera, calls
// SetRecordingMode, does D3D immediate-context readback or ADB keep-alive.
// Stale/exited sessions keep the last frame behind an explicit grey "not live"
// overlay — the window never fakes a live view.
//
// .135b fixes: definite-init for ABI outs; preview EPOCH + successful-StorePreview
// count drive the content FPS (frameSerial gaps include submit failures and reset per
// flow — they are NOT a success count); bitmap failures show an error overlay instead
// of hiding it; the native last-error is labelled HISTORICAL; timer/closed handlers
// are named methods unsubscribed on close.
//
// Display-only mirror controls: eye view selector (双眼并排/左眼/右眼 — visibility only,
// NOT the final composited HMD image), fullscreen/windowed toggle (Esc exits fullscreen
// only), collapsible status panel. Closing the window stops ONLY this window's preview
// timer; it never calls ExitVR or any host session/native-camera entry point.
//
// shared-monitor-ui (this card): OnTick starts with ONE read-only
// _host.CaptureSharedGpuMonitorSample(). When the snapshot reports SharedSelected the
// tick renders the shared-STATUS branch and returns — the grey overlay ALWAYS states
// that PC mirroring is not provided; NO pixels, bitmap, readback, DumpSceneTexture,
// NC01 or WGC are ever touched here, and counter progress NEVER hides the overlay.
// The shared distinct-content rate comes ONLY from StatsB.uniqueLiveLayerOk over
// monotonic QPC windows via the PURE SharedRateState/SharedRateResult/
// AdvanceSharedRate logic (no WPF/native/host getters/IO/timer/DateTime inside),
// which QA can drive standalone through the internal Draft + Seal form.
// Invalid or ReadError snapshots (and helper exceptions / null samples) are intercepted
// BEFORE both branches: ResetSharedRate + residual CPU bitmaps cleared + the existing
// overlay/status show "监控读取失败/失效" with the Reason, and this tick runs NO old CPU
// native/getter/preview read (no retry). Valid non-shared snapshots ResetSharedRate and
// continue the OLD CPU branch unchanged
// except one label: its successful preview-store rate is a CPU预览更新 rate — it is
// NOT the shared distinct-layer rate and NOT a final presentation FPS.
// shared-monitor-ui-repair: the pure rate key is DUAL (host/main reference + producer
// reference, ReferenceEquals only); equal-or-backward QPC and counter regression reset
// to Unknown WITHOUT seeding the bad observation; the shared overlay uses SHORT wrapping
// lines (technical boundaries/window/300ms-120/count definition stay in details).
using System;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace VRSIM
{
    internal sealed class VRMonitorWindow : Window
    {
        private readonly VRSIMPlugin _host;
        private readonly DispatcherTimer _timer;
        private readonly Image _imgL, _imgR;
        private readonly TextBlock _ovL, _ovR;
        private readonly Border _ovBorderL, _ovBorderR;
        private readonly TextBlock _status;
        private readonly Grid _cellL, _cellR;
        private readonly ColumnDefinition _colL, _colR;
        private readonly ScrollViewer _scroll;
        private readonly ComboBox _eyeSelector;
        private readonly CheckBox _showDetailsChk;
        private readonly Button _fullscreenBtn;

        private bool _isFullscreen;
        private WindowStyle _prevWindowStyle;
        private ResizeMode _prevResizeMode;
        private WindowState _prevWindowState;

        private ulong _lastBitmapFs = ulong.MaxValue;
        private string _bitmapError;
        private int _previewEpoch = -1;
        private ulong _storeRefSerial;          // last sampled successful-store count
        private DateTime _storeRefAt = DateTime.MinValue;
        private double _contentFps;

        // shared-route rate anchor — pure scalars ONLY; reset on non-shared ticks
        // and on any invalid/key-changed/regressed observation (see AdvanceSharedRate).
        private SharedRateState _sharedRate;

        // ================= PURE SHARED RATE MATH (QA-target) =================
        // Mutable tracking state. SharedRateResult below is the immutable output.
        internal struct SharedRateState
        {
            public bool Active;
            public object KeyMainIdentity;          // ReferenceEquals key ONLY — never dereferenced
            public object KeyProducerIdentity;      // second independent ReferenceEquals key — never dereferenced
            public int KeyMainGeneration, KeyActiveFlowGeneration, KeyStartGeneration;
            public ulong KeyEpoch;                  // StatsB.sessionEpoch at seed
            public bool HasFirstValidQpc; public long FirstValidQpc;
            public bool HasLastObservedNewQpc; public long LastObservedNewQpc;
            public bool HasPreviousCount; public ulong PreviousSampleCount;
            public bool HasLastSampleQpc; public long LastSampleQpc;   // monotonic-QPC guard
            public bool HasWindowAnchor; public ulong WindowAnchorCount; public long WindowAnchorQpc;
            public bool HasRate; public double RateFps;               // current measured rate (may be 0.0)
            public bool HasHistory; public double HistoryFps;         // last POSITIVE measured rate
            public bool HasLastWindow; public ulong LastWindowDelta; public long LastWindowSpanQpc;
            public double LastWindowRateFps;
        }

        // Immutable observation result. Constructed only via the static factories.
        internal sealed class SharedRateResult
        {
            public readonly bool Eligible;          // false = reset to unknown (never fabricate 0)
            public readonly bool Seeding;           // valid observation, first >=1.0s window not complete
            public readonly bool Stalled;           // >=1.0s with no observed count increase
            public readonly bool RateKnown; public readonly double RateFps;
            public readonly bool AgeKnown; public readonly double AgeMs;
            public readonly bool HistoryKnown; public readonly double HistoryFps;
            public readonly bool LastWindowKnown; public readonly bool LastWindowZeroDelta;
            public readonly ulong LastWindowDelta; public readonly double LastWindowSpanMs;
            public readonly double LastWindowRateFps;
            public readonly ulong ObservedUniqueCount;   // StatsB.uniqueLiveLayerOk at this observation
            public readonly string Note;                 // machine-readable classification, never null

            private SharedRateResult(bool eligible, bool seeding, bool stalled, bool rateKnown, double rateFps,
                bool ageKnown, double ageMs, bool historyKnown, double historyFps, bool lastWindowKnown,
                bool lastWindowZeroDelta, ulong lastWindowDelta, double lastWindowSpanMs, double lastWindowRateFps,
                ulong observedUniqueCount, string note)
            {
                Eligible = eligible; Seeding = seeding; Stalled = stalled;
                RateKnown = rateKnown; RateFps = rateFps;
                AgeKnown = ageKnown; AgeMs = ageMs;
                HistoryKnown = historyKnown; HistoryFps = historyFps;
                LastWindowKnown = lastWindowKnown; LastWindowZeroDelta = lastWindowZeroDelta;
                LastWindowDelta = lastWindowDelta; LastWindowSpanMs = lastWindowSpanMs;
                LastWindowRateFps = lastWindowRateFps;
                ObservedUniqueCount = observedUniqueCount; Note = note ?? "";
            }

            public static SharedRateResult Unknown(string note)
            {
                return new SharedRateResult(false, false, false, false, 0.0, false, 0.0, false, 0.0,
                    false, false, 0UL, 0.0, 0.0, 0UL, note);
            }

            public static SharedRateResult Observation(bool seeding, bool stalled, bool rateKnown, double rateFps,
                double ageMs, bool historyKnown, double historyFps, bool lastWindowKnown, bool lastWindowZeroDelta,
                ulong lastWindowDelta, double lastWindowSpanMs, double lastWindowRateFps,
                ulong observedUniqueCount, string note)
            {
                return new SharedRateResult(true, seeding, stalled, rateKnown, rateFps, true, ageMs,
                    historyKnown, historyFps, lastWindowKnown, lastWindowZeroDelta, lastWindowDelta,
                    lastWindowSpanMs, lastWindowRateFps, observedUniqueCount, note);
            }
        }

        /// <summary>PURE distinct-live-content rate advance: no WPF, no native, no host
        /// getters, no IO, no timers, no DateTime. The ONLY content counter is
        /// StatsB.uniqueLiveLayerOk (prepared/commit/rawAccepted/runtime frameSerial/
        /// CPU PreviewStoreCount may NEVER substitute it). DUAL independent reference keys:
        /// mainIdentity (the host instance) AND sample.ProducerIdentity — each is used ONLY
        /// via ReferenceEquals, no member is ever read through either; a changed reference
        /// on EITHER is a new key and clears the old rate. A rate window needs a
        /// full >=1.0s monotonic QPC span; every completed window (including delta 0)
        /// advances the anchor. First observation = seeding (sampling, rate UNKNOWN —
        /// never a manufactured 0). Key change / counter regression / non-monotonic (equal
        /// or backward) QPC reset the WHOLE state to Unknown WITHOUT seeding the bad
        /// observation; the next valid observation seeds. Unknown clears rate, age and
        /// history. Any non-eligible sample (invalid / read error / null identity reference /
        /// not shared / not bound / not live / no StatsB / stopped /
        /// bad frequency / bad sampleQpc / zero epoch) resets the state to unknown.</summary>
        internal static SharedRateResult AdvanceSharedRate(ref SharedRateState state, object mainIdentity,
            VRSIMPlugin.SharedGpuMonitorSample sample, long qpcFrequency)
        {
            if (qpcFrequency <= 0L)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("invalid_qpc_frequency");
            }
            if (sample == null)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("sample_missing");
            }
            if (mainIdentity == null || sample.ProducerIdentity == null)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("identity_reference_missing");
            }
            if (sample.ReadError)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("snapshot_read_error:" + OrUnkn(sample.Reason));
            }
            if (!sample.Valid || !sample.SharedSelected || !sample.BoundValid || !sample.LiveValid)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("sample_not_rate_eligible:" + OrUnkn(sample.Reason));
            }
            if (!sample.StatsB.HasValue)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("stats_b_absent");
            }
            if (sample.StatsB.Value.sessionEpoch == 0UL)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("stats_epoch_zero");
            }
            if (sample.SampleQpc <= 0L)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("sample_qpc_invalid");
            }
            if (IsStoppedPhase(sample.Phase))
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("stopped_phase:" + OrUnkn(sample.Phase));
            }

            var b = sample.StatsB.Value;
            long qpc = sample.SampleQpc;
            ulong count = b.uniqueLiveLayerOk;

            bool keySame = state.Active
                && ReferenceEquals(state.KeyMainIdentity, mainIdentity)
                && ReferenceEquals(state.KeyProducerIdentity, sample.ProducerIdentity)
                && state.KeyMainGeneration == sample.MainGeneration
                && state.KeyActiveFlowGeneration == sample.ActiveFlowGeneration
                && state.KeyStartGeneration == sample.OriginalHostedStartGeneration
                && state.KeyEpoch == b.sessionEpoch;

            // First valid observation of a key (or reseed after any invalidation):
            // seed anchor + prevCount + firstValidQpc. This is NOT "new content observed".
            if (!keySame)
            {
                state = default(SharedRateState);
                SeedSharedRateState(ref state, mainIdentity, sample, b.sessionEpoch, qpc, count);
                return EvaluateSharedRate(ref state, qpc, qpcFrequency, count, "seed_sampling_first_window_pending");
            }

            // Non-monotonic QPC on the same key — EQUAL or backward both count — resets the
            // WHOLE state to Unknown WITHOUT seeding this bad observation; the next valid
            // sample seeds fresh (the !keySame path, because Active is now false).
            if (state.HasLastSampleQpc && qpc <= state.LastSampleQpc)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("qpc_non_monotonic_reset");
            }
            // Counter regression: same reset semantics — the regressing observation is NOT a seed.
            if (state.HasPreviousCount && count < state.PreviousSampleCount)
            {
                state = default(SharedRateState);
                return SharedRateResult.Unknown("counter_regression_reset");
            }

            state.LastSampleQpc = qpc;
            state.HasLastSampleQpc = true;

            // Independent previousSampleCount: the new-content age advances ONLY on a real
            // increase over the PREVIOUS observation — never on the window anchor alone.
            bool increased = state.HasPreviousCount && count > state.PreviousSampleCount;
            if (increased) { state.HasLastObservedNewQpc = true; state.LastObservedNewQpc = qpc; }
            state.PreviousSampleCount = count;

            if (!state.HasWindowAnchor)
            {
                state.HasWindowAnchor = true;
                state.WindowAnchorCount = count;
                state.WindowAnchorQpc = qpc;
            }
            else
            {
                long spanQpc = qpc - state.WindowAnchorQpc;
                if (spanQpc >= qpcFrequency)   // full >=1.0s window; regression already excluded above
                {
                    ulong delta = count - state.WindowAnchorCount;   // safe: monotonic within a key
                    double spanSec = (double)spanQpc / qpcFrequency; // >= 1.0 by the check above
                    double rate = delta / spanSec;
                    state.HasRate = true;
                    state.RateFps = rate;
                    state.HasLastWindow = true;
                    state.LastWindowDelta = delta;
                    state.LastWindowSpanQpc = spanQpc;
                    state.LastWindowRateFps = rate;
                    if (rate > 0.0) { state.HasHistory = true; state.HistoryFps = rate; }
                    // EVERY completed window advances the anchor — including delta 0.
                    state.WindowAnchorCount = count;
                    state.WindowAnchorQpc = qpc;
                }
            }

            return EvaluateSharedRate(ref state, qpc, qpcFrequency, count, "observation");
        }

        private static void SeedSharedRateState(ref SharedRateState state, object mainIdentity,
            VRSIMPlugin.SharedGpuMonitorSample sample, ulong epoch, long qpc, ulong count)
        {
            state.Active = true;
            state.KeyMainIdentity = mainIdentity;
            state.KeyProducerIdentity = sample.ProducerIdentity;
            state.KeyMainGeneration = sample.MainGeneration;
            state.KeyActiveFlowGeneration = sample.ActiveFlowGeneration;
            state.KeyStartGeneration = sample.OriginalHostedStartGeneration;
            state.KeyEpoch = epoch;
            state.FirstValidQpc = qpc; state.HasFirstValidQpc = true;
            state.PreviousSampleCount = count; state.HasPreviousCount = true;
            state.WindowAnchorCount = count; state.WindowAnchorQpc = qpc; state.HasWindowAnchor = true;
            state.LastSampleQpc = qpc; state.HasLastSampleQpc = true;
            // LastObservedNewQpc and any rate stay UNSET on a seed.
        }

        // Age basis: with an observed increase → lastObservedNewQpc; until the first
        // increase on this key → firstValidQpc. >=1.0s without an increase is STALE and
        // clears any positive current rate into history. A measured 0.0 window stays a
        // legitimate current measurement (not failure due to a missing CPU preview).
        private static SharedRateResult EvaluateSharedRate(ref SharedRateState state, long qpc,
            long qpcFrequency, ulong count, string note)
        {
            long ageRef = state.HasLastObservedNewQpc ? state.LastObservedNewQpc : state.FirstValidQpc;
            double ageMs = qpc > ageRef ? (qpc - ageRef) * 1000.0 / qpcFrequency : 0.0;
            bool stalled = ageMs >= 1000.0;
            if (stalled && state.HasRate && state.RateFps > 0.0)
            {
                if (!state.HasHistory || state.RateFps > state.HistoryFps) { state.HasHistory = true; state.HistoryFps = state.RateFps; }
                state.HasRate = false;   // stale clears the current positive live rate
            }
            bool rateKnown = state.HasRate && !stalled;
            bool seeding = !state.HasRate && !stalled;
            return SharedRateResult.Observation(seeding, stalled, rateKnown, rateKnown ? state.RateFps : 0.0,
                ageMs, state.HasHistory, state.HistoryFps,
                state.HasLastWindow, state.HasLastWindow && state.LastWindowDelta == 0UL,
                state.LastWindowDelta,
                state.HasLastWindow ? (double)state.LastWindowSpanQpc * 1000.0 / qpcFrequency : 0.0,
                state.HasLastWindow ? state.LastWindowRateFps : 0.0,
                count, note);
        }

        private static bool IsStoppedPhase(string phase)
        {
            return string.Equals(phase, "stopped", StringComparison.Ordinal)
                || string.Equals(phase, "inactive", StringComparison.Ordinal)
                || string.Equals(phase, "failure", StringComparison.Ordinal);
        }

        // ================= UI =================

        public VRMonitorWindow(VRSIMPlugin host)
        {
            _host = host;
            Title = "VRSIM 实时监控（SIM 提交预览 — 非最终头显合成）";
            Width = 1050; Height = 650;
            ResizeMode = ResizeMode.CanResize;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            Background = Brushes.Black;

            var root = new Grid { Margin = new Thickness(8) };
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });                       // toolbar
            root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });   // images
            root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });                       // status

            var toolbar = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 4) };
            toolbar.Children.Add(new TextBlock { Text = "视图:", Foreground = Brushes.WhiteSmoke, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 4, 0) });
            _eyeSelector = new ComboBox { Width = 110, VerticalAlignment = VerticalAlignment.Center };
            _eyeSelector.Items.Add("双眼并排");
            _eyeSelector.Items.Add("左眼");
            _eyeSelector.Items.Add("右眼");
            _eyeSelector.SelectedIndex = 0;
            _eyeSelector.SelectionChanged += OnEyeSelectionChanged;
            toolbar.Children.Add(_eyeSelector);
            _fullscreenBtn = new Button { Content = "全屏", Margin = new Thickness(8, 0, 0, 0), Padding = new Thickness(8, 2, 8, 2), VerticalAlignment = VerticalAlignment.Center };
            _fullscreenBtn.Click += OnFullscreenToggleClick;
            toolbar.Children.Add(_fullscreenBtn);
            _showDetailsChk = new CheckBox { Content = "显示状态详情", Foreground = Brushes.WhiteSmoke, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 0, 0) };
            _showDetailsChk.Checked += OnShowDetailsToggled;
            _showDetailsChk.Unchecked += OnShowDetailsToggled;
            toolbar.Children.Add(_showDetailsChk);
            Grid.SetRow(toolbar, 0);
            root.Children.Add(toolbar);

            var imgs = new Grid();
            _colL = new ColumnDefinition();
            _colR = new ColumnDefinition();
            imgs.ColumnDefinitions.Add(_colL);
            imgs.ColumnDefinitions.Add(_colR);
            _cellL = BuildEyeCell("左眼：SIM 提交预览", out _imgL, out _ovL, out _ovBorderL);
            Grid.SetColumn(_cellL, 0);
            _cellR = BuildEyeCell("右眼：SIM 提交预览", out _imgR, out _ovR, out _ovBorderR);
            Grid.SetColumn(_cellR, 1);
            imgs.Children.Add(_cellL);
            imgs.Children.Add(_cellR);
            Grid.SetRow(imgs, 1);
            root.Children.Add(imgs);

            _scroll = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Margin = new Thickness(0, 8, 0, 0), MaxHeight = 240, Visibility = Visibility.Collapsed };
            _status = new TextBlock
            {
                Foreground = Brushes.Gainsboro,
                FontFamily = new FontFamily("Consolas"),
                FontSize = 12,
                TextWrapping = TextWrapping.NoWrap
            };
            _scroll.Content = _status;
            Grid.SetRow(_scroll, 2);
            root.Children.Add(_scroll);

            Content = root;
            PreviewKeyDown += OnPreviewKeyDown;

            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(300) }; // UI refresh only — never Sleep
            _timer.Tick += OnTimerTick;
            _timer.Start();
            Closed += OnWindowClosed;
            ShowOverlay("尚无新帧：还没有任何成功提交的立体帧（非实时）");
        }

        private void OnTimerTick(object sender, EventArgs e) { OnTick(); }

        private void OnWindowClosed(object sender, EventArgs e)
        {
            // Stops ONLY this window's preview/UI hooks — never ExitVR or host session calls.
            try { _timer.Stop(); _timer.Tick -= OnTimerTick; } catch { }
            try { _eyeSelector.SelectionChanged -= OnEyeSelectionChanged; } catch { }
            try { _fullscreenBtn.Click -= OnFullscreenToggleClick; } catch { }
            try { _showDetailsChk.Checked -= OnShowDetailsToggled; _showDetailsChk.Unchecked -= OnShowDetailsToggled; } catch { }
            try { PreviewKeyDown -= OnPreviewKeyDown; } catch { }
            try { Closed -= OnWindowClosed; } catch { }
        }

        private void OnEyeSelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            // Visibility-only view selection; side-by-side is two independent preview cells, not a blend.
            int i = _eyeSelector.SelectedIndex;
            _cellL.Visibility = i == 2 ? Visibility.Collapsed : Visibility.Visible;
            _cellR.Visibility = i == 1 ? Visibility.Collapsed : Visibility.Visible;
            var star = new GridLength(1, GridUnitType.Star);
            _colL.Width = i == 2 ? GridLength.Auto : star;
            _colR.Width = i == 1 ? GridLength.Auto : star;
        }

        private void OnShowDetailsToggled(object sender, RoutedEventArgs e)
        {
            _scroll.Visibility = _showDetailsChk.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
        }

        private void OnFullscreenToggleClick(object sender, RoutedEventArgs e)
        {
            if (_isFullscreen) { ExitFullscreen(); return; }
            _prevWindowState = WindowState;
            _prevWindowStyle = WindowStyle;
            _prevResizeMode = ResizeMode;
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.NoResize;
            WindowState = WindowState.Maximized;
            _isFullscreen = true;
            _fullscreenBtn.Content = "退出全屏";
        }

        private void ExitFullscreen()
        {
            if (!_isFullscreen) return;
            WindowState = _prevWindowState;
            WindowStyle = _prevWindowStyle;
            ResizeMode = _prevResizeMode;
            _isFullscreen = false;
            _fullscreenBtn.Content = "全屏";
        }

        private void OnPreviewKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Escape && _isFullscreen)
            {
                ExitFullscreen();
                e.Handled = true;
            }
        }

        private static Grid BuildEyeCell(string label, out Image img, out TextBlock ovText, out Border ovBorder)
        {
            var cell = new Grid { Margin = new Thickness(4) };
            cell.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            cell.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            var lbl = new TextBlock { Text = label, Foreground = Brushes.WhiteSmoke, FontSize = 13, Margin = new Thickness(0, 0, 0, 4) };
            Grid.SetRow(lbl, 0);
            cell.Children.Add(lbl);
            var canvas = new Grid();
            img = new Image { Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch };
            canvas.Children.Add(img);
            ovText = new TextBlock
            {
                Foreground = Brushes.White,
                FontSize = 16,
                TextWrapping = TextWrapping.Wrap,
                TextAlignment = TextAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(12, 0, 12, 0)
            };
            ovBorder = new Border { Background = new SolidColorBrush(Color.FromArgb(0xB0, 0x40, 0x40, 0x40)), Child = ovText };
            canvas.Children.Add(ovBorder);
            Grid.SetRow(canvas, 1);
            cell.Children.Add(canvas);
            return cell;
        }

        private void ShowOverlay(string text)
        {
            _ovL.Text = text; _ovR.Text = text;
            _ovBorderL.Visibility = Visibility.Visible;
            _ovBorderR.Visibility = Visibility.Visible;
        }

        private void HideOverlay()
        {
            _ovBorderL.Visibility = Visibility.Collapsed;
            _ovBorderR.Visibility = Visibility.Collapsed;
        }

        private void ResetSharedRate() { _sharedRate = default(SharedRateState); }

        // Drops any residual CPU preview bitmaps so a later CPU branch rebuilds cleanly;
        // shared mode NEVER has pixels to show.
        private void ClearCpuPreviewImages()
        {
            if (_imgL.Source != null || _imgR.Source != null)
            {
                _imgL.Source = null;
                _imgR.Source = null;
            }
            _lastBitmapFs = ulong.MaxValue;
            _bitmapError = null;
        }

        private void OnTick()
        {
            // ---- ONE read-only shared snapshot first (existing UI dispatcher only) ----
            VRSIMPlugin.SharedGpuMonitorSample snap = null;
            bool captureThrew = false;
            string captureError = null;
            try { snap = _host.CaptureSharedGpuMonitorSample(); }
            catch (Exception ex) { captureThrew = true; captureError = ex.GetType().Name + ": " + ex.Message; }

            if (captureThrew || snap == null)
            {
                // Helper read failure: reset the shared rate, show the failure, NO retry here.
                ResetSharedRate();
                ClearCpuPreviewImages();
                ShowOverlay("共享GPU监控读取失败（无快照，本 tick 不重试）\n" + (captureError ?? "sample_null") +
                    "\n电脑画面镜像尚未提供");
                _status.Text = "共享GPU监控：CaptureSharedGpuMonitorSample 读取失败: " + (captureError ?? "sample_null") +
                    "（不自动重试；下个 300ms tick 仅再采一次；共享速率锚点已复位）";
                return;
            }

            // shared-monitor-ui-repair: an invalid or read-error snapshot is intercepted
            // BEFORE both branches — the rate anchor resets, residual CPU bitmaps are
            // cleared, the failure + Reason go to the existing overlay/status, and this
            // tick NEVER runs an old CPU native/getter/preview read. No retry here.
            if (!snap.Valid || snap.ReadError)
            {
                ResetSharedRate();
                ClearCpuPreviewImages();
                ShowOverlay("监控读取失败/失效\n原因：" + OrUnkn(snap.Reason) +
                    "\n本 tick 未读取 CPU/preview（不自动重试）\n电脑画面镜像尚未提供");
                var fsb = new StringBuilder();
                fsb.Append("共享GPU监控读取失败/失效详情: valid=").Append(B(snap.Valid))
                  .Append(" readError=").Append(B(snap.ReadError))
                  .Append(" sharedSelected=").Append(B(snap.SharedSelected))
                  .Append(" reason=").Append(OrUnkn(snap.Reason))
                  .Append(" phase=").Append(OrUnkn(snap.Phase))
                  .Append(" failure=").Append(OrDash(EmptyToNull(snap.Failure)))
                  .Append(" mainGen=").Append(snap.MainGeneration)
                  .Append(" flowGen=").Append(snap.ActiveFlowGeneration)
                  .Append(" startGen=").Append(snap.OriginalHostedStartGeneration)
                  .Append(" sampleQpc=").Append(snap.SampleQpc)
                  .Append("（速率锚点/年龄/历史已复位；速率/年龄无效；本 tick 无 CPU 读取）");
                _status.Text = fsb.ToString();
                return;
            }

            if (snap.SharedSelected)
            {
                OnTickShared(snap);
                return;
            }

            // Valid non-shared: reset the shared rate, then the OLD CPU monitor branch (unchanged).
            ResetSharedRate();

            var ci = CultureInfo.InvariantCulture;
            var errs = new StringBuilder();
            var now = DateTime.UtcNow;

            int st = -1;
            try { st = _host.NativeStateForMonitor(); } catch (Exception ex) { AppendErr(errs, "state", ex); }
            string phase = "?";
            try { phase = _host.MonitorPhase(); } catch (Exception ex) { AppendErr(errs, "phase", ex); }

            // ABI outs are DEFINITELY initialized — a read failure can never surface stale
            // garbage as valid data (haveTiming=false is shown explicitly below).
            ulong frameSerial = 0; long periodNs = 0; int shouldRender = 0, trackingValid = 0;
            bool haveTiming = false;
            try { haveTiming = _host.TryGetFrameTiming(out frameSerial, out periodNs, out shouldRender, out trackingValid); }
            catch (Exception ex) { AppendErr(errs, "frameTiming", ex); }

            VRSIMPlugin.VRSIMStereoStats? stats = null;
            try { stats = _host.ReadStereoStats(); } catch (Exception ex) { AppendErr(errs, "stereoStats", ex); }

            byte[] l = null, r = null; int w = 0, h = 0, stride = 0; ulong fs = 0, ps = 0; DateTime utc = DateTime.MinValue;
            int epoch = 0;
            bool havePreview = false;
            try { havePreview = _host.TryGetPreview(out l, out r, out w, out h, out stride, out fs, out ps, out utc, out epoch); }
            catch (Exception ex) { AppendErr(errs, "preview", ex); }

            ulong storeCount = 0;
            try { storeCount = _host.PreviewStoreCountForMonitor(); } catch (Exception ex) { AppendErr(errs, "storeCount", ex); }

            bool viewsValid = false;
            var vl = default(VRSIMPlugin.VRSIMEyeView);
            var vr = default(VRSIMPlugin.VRSIMEyeView);
            try
            {
                VRSIMPlugin.VRSIMEyeView tl, tr;
                if (_host.TryGetViews(out tl, out tr) && EyeViewSane(tl) && EyeViewSane(tr))
                { vl = tl; vr = tr; viewsValid = true; }
            }
            catch (Exception ex) { AppendErr(errs, "views", ex); }

            string nativeVer = "?", producerInfo = "n/a";
            try { nativeVer = _host.NativeVersionInfo(); } catch (Exception ex) { AppendErr(errs, "nativeVersion", ex); }
            try { producerInfo = _host.ProducerMonitorInfo(); } catch (Exception ex) { AppendErr(errs, "producerInfo", ex); }

            // ---- epoch / reset-safe content FPS from SUCCESSFUL STORE COUNT ----
            // (frameSerial gaps include submit failures and restart at 0 per flow — never
            // used as a success counter here; ulong subtraction only after reset checks.)
            bool flowReset = epoch != _previewEpoch || !havePreview || storeCount < _storeRefSerial ||
                             _storeRefAt == DateTime.MinValue;
            if (flowReset)
            {
                _previewEpoch = epoch;
                _lastBitmapFs = ulong.MaxValue;      // repaint the first frame of the new flow
                _storeRefSerial = storeCount;
                _storeRefAt = now;
                _contentFps = 0;
            }
            else if (storeCount != _storeRefSerial)
            {
                double el = (now - _storeRefAt).TotalSeconds;
                if (el >= 0.5)
                {
                    _contentFps = (double)(storeCount - _storeRefSerial) / el;
                    _storeRefSerial = storeCount;
                    _storeRefAt = now;
                }
            }
            else if ((now - _storeRefAt).TotalSeconds >= 1.0)
                _contentFps = 0;                     // stopped window → 0, honest

            double ageMs = havePreview ? (now - utc).TotalMilliseconds : double.PositiveInfinity;
            bool sessionLive = st == 5 || st == 6;
            bool frozen = havePreview && ageMs > 2000;

            // ---- bitmap only for a NEW serial of the CURRENT flow ----
            if (havePreview && fs != _lastBitmapFs)
            {
                try
                {
                    var bl = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgra32, null, l, stride);
                    var br = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgra32, null, r, stride);
                    _imgL.Source = FreezeIt(bl);
                    _imgR.Source = FreezeIt(br);
                    _lastBitmapFs = fs;
                    _bitmapError = null;
                }
                catch (Exception ex)
                {
                    // failure keeps the grey error overlay — NEVER hideOverlay on a bad decode
                    _bitmapError = ex.GetType().Name + ": " + ex.Message;
                    AppendErr(errs, "bitmap", ex);
                }
            }

            if (!havePreview) ShowOverlay("尚无新帧：还没有任何成功提交的立体帧（非实时）");
            else if (!sessionLive) ShowOverlay("静态：XR 会话非 VISIBLE/FOCUSED（state=" + VRSIMPlugin.StateName(st) +
                "）— 保留末帧，非实时");
            else if (_bitmapError != null) ShowOverlay("监控图像解码失败: " + _bitmapError + "（保留上一图像，非实时）");
            else if (frozen) ShowOverlay("静态警告：末帧已 " + ageMs.ToString("0", ci) + "ms 无新帧（非实时）");
            else if (haveTiming && trackingValid == 0) ShowOverlay("跟踪无效/保留图像（native trackingValid=0，非实时）");
            else HideOverlay();

            string pid;
            try { pid = Process.GetCurrentProcess().Id.ToString(ci); } catch { pid = "?"; }

            var sb = new StringBuilder();
            sb.Append("版本: plugin v").Append(VRSIMPlugin.PluginVersionForMonitor)
              .Append(" | native ").Append(nativeVer)
              .Append(" | pid ").Append(pid).AppendLine();
            sb.Append("会话: XR state=").Append(VRSIMPlugin.StateName(st)).Append('(').Append(st).Append(')')
              .Append(" | 阶段=").Append(OrUnkn(phase))
              .Append(" | 流=").Append(_host.DiagnosticFlowActive ? "diagnostic" : "normal")
              .Append(" | vrActive=").Append(_host.VrActiveForMonitor ? "Y" : "N")
              .Append(" | previewEpoch=").Append(epoch).AppendLine();
            if (haveTiming)
            {
                double estHz = periodNs > 0 ? 1e9 / periodNs : double.NaN;
                sb.Append("跟踪: trackingValid=").Append(trackingValid)
                  .Append(" shouldRender=").Append(shouldRender)
                  .Append(" frameSerial=").Append(frameSerial)
                  .Append(" | XR显示周期=").Append((periodNs / 1e6).ToString("0.00", ci)).Append("ms")
                  .Append(" → 推算刷新率=").Append(double.IsNaN(estHz) ? "n/a" : estHz.ToString("0.0", ci) + "Hz").AppendLine();
            }
            else sb.Append("跟踪: frame timing 读取失败（无数据，不冒充有效值）").AppendLine();
            sb.Append("内容: 成功提交计数=").Append(storeCount.ToString(ci))
              .Append(" 提交帧 frameSerial=").Append(havePreview ? fs.ToString(ci) : "-")
              .Append(" poseSerial=").Append(havePreview ? ps.ToString(ci) : "-")
              .Append(" | 帧龄=").Append(double.IsPositiveInfinity(ageMs) ? "n/a" : ageMs.ToString("0", ci) + "ms")
              .Append(frozen ? " [冻结警告 >2s]" : "")
              .Append(" | CPU预览更新≈").Append(_contentFps.ToString("0.0", ci))
              .Append("（按成功提交预览计数推算的 CPU 预览更新率；非共享GPU真实帧率、非最终呈现FPS；frameSerial缺口含提交失败；静止/停帧≥1s显示0）").AppendLine();
            if (stats.HasValue)
            {
                var v = stats.Value;
                sb.Append("提交: ok=").Append(v.submitOk.ToString(ci))
                  .Append(" fail=").Append(v.submitFail.ToString(ci))
                  .Append(" uploads=").Append(v.d3dUploads.ToString(ci))
                  .Append(" reused=").Append(v.reusedFrames.ToString(ci))
                  .Append(" dropped=").Append(v.framesDropped.ToString(ci))
                  .Append(" | preview ").Append(w).Append('x').Append(h).Append(" stride=").Append(stride)
                  .Append(" | producer[").Append(producerInfo).Append(']').AppendLine();
            }
            else sb.Append("提交: stats 读取失败（无数据）").AppendLine();
            if (viewsValid)
            {
                sb.Append("每眼FOV(runtime views有效, 左/右/上/下 °): L[")
                  .Append(Deg(vl.fovLeft)).Append('/').Append(Deg(vl.fovRight)).Append('/')
                  .Append(Deg(vl.fovUp)).Append('/').Append(Deg(vl.fovDown))
                  .Append("] R[").Append(Deg(vr.fovLeft)).Append('/').Append(Deg(vr.fovRight)).Append('/')
                  .Append(Deg(vr.fovUp)).Append('/').Append(Deg(vr.fovDown)).Append(']').AppendLine();
            }
            else sb.Append("每眼FOV: runtime views 无效或尚未产生（无数据）").AppendLine();
            try
            {
                string err = _host.NativeErrorForMonitor();
                if (!string.IsNullOrEmpty(err))
                    sb.Append("native历史错误(可能为上一轮残留如 HMD not available，不代表当前连接状态): ").AppendLine(err);
            }
            catch (Exception ex) { AppendErr(errs, "lastError", ex); }
            if (errs.Length > 0)
                sb.Append("错误(读取失败明确显示，未吞掉): ").AppendLine(errs.ToString());
            string profileText = "n/a";
            try { profileText = CaptureProfile.DiagnosticText; }
            catch (Exception ex) { AppendErr(errs, "captureProfile", ex); }
            sb.Append("采集配置(与下方实际预览尺寸分开显示): ").AppendLine(profileText);
            sb.Append("说明: 图像 = SIM 提交预览（")
              .Append(havePreview ? w + "x" + h : "unknown：尚无成功提交的预览帧")
              .Append("），不是最终头显合成；灰色遮罩 = 非实时/无新帧。");
            _status.Text = sb.ToString();
        }

        // ---- SHARED-STATUS BRANCH: snapshot + pure rate math are the ONLY inputs.
        // No native/host reads beyond the already-taken snapshot, no pixels, no new
        // timers/threads/windows/menu items. ----
        private void OnTickShared(VRSIMPlugin.SharedGpuMonitorSample snap)
        {
            // residual CPU bitmaps must not masquerade as shared content (rebuildable later)
            ClearCpuPreviewImages();

            SharedRateResult rate;
            try
            {
                rate = AdvanceSharedRate(ref _sharedRate, _host, snap, Stopwatch.Frequency);
            }
            catch (Exception ex)
            {
                ResetSharedRate();
                ShowOverlay("共享GPU速率计算异常（不重试）\n" + ex.GetType().Name + ": " + ex.Message +
                    "\n电脑画面镜像尚未提供");
                _status.Text = "共享GPU监控：AdvanceSharedRate 异常 " + ex.GetType().Name + ": " + ex.Message +
                    "（速率锚点已复位；快照字段仍列于下）" + BuildSharedDetail(snap, null);
                return;
            }

            var ci = CultureInfo.InvariantCulture;
            string stateText = ClassifySharedState(snap, rate);

            // SHORT lines only — each eye cell is ~500px wide by default; long technical
            // boundaries, window facts, 300ms/120 notes and the true count definition stay
            // in the expandable details.
            string dimsText;
            if (snap.ActualWidth.HasValue && snap.ActualHeight.HasValue)
                dimsText = "实际场景：" + snap.ActualWidth.Value.ToString(ci) + "×" + snap.ActualHeight.Value.ToString(ci);
            else
                dimsText = "配置：" + snap.CompiledRequestedEdge.ToString(ci) + "×" + snap.CompiledRequestedEdge.ToString(ci) + "（未确认）";

            string fpsText;
            if (rate != null && rate.Eligible && rate.RateKnown)
                fpsText = "新内容：" + rate.RateFps.ToString("0.0", ci) + " fps";
            else if (rate != null && rate.Eligible && rate.Stalled)
                fpsText = rate.LastWindowKnown && rate.LastWindowZeroDelta
                    ? "新内容：更新停滞（实测0 fps）"
                    : "新内容：更新停滞";
            else if (rate != null && rate.Eligible && rate.Seeding)
                fpsText = "新内容：采样中";
            else
                fpsText = "新内容：未知";

            string xrText;
            if (snap.HasFrameTiming && snap.DisplayPeriodNs > 0)
                xrText = "XR报告：" + (1e9 / snap.DisplayPeriodNs).ToString("0.0", ci) + " Hz（非内容帧率）";
            else
                xrText = "XR报告：未知";

            // A seed has NOT observed any count increase yet — never label it "观察到新内容".
            string ageText = "距观察到新内容：未知";
            if (rate != null && rate.Eligible && rate.AgeKnown)
                ageText = _sharedRate.HasLastObservedNewQpc
                    ? "距观察到新内容：" + rate.AgeMs.ToString("0", ci) + " ms"
                    : "尚未观察到新内容增量\n首次采样至今：" + rate.AgeMs.ToString("0", ci) + " ms";

            var ov = new StringBuilder();
            ov.Append("共享GPU：").Append(stateText).AppendLine();
            ov.Append(dimsText).AppendLine();
            ov.Append(fpsText).AppendLine();
            ov.Append(xrText).AppendLine();
            ov.Append(ageText).AppendLine();
            ov.Append("共享GPU状态正在监控").AppendLine();
            ov.Append("电脑画面镜像尚未提供");
            // NEVER HideOverlay in shared mode — counter progress is not image evidence.
            ShowOverlay(ov.ToString());

            _status.Text = "共享GPU监控快照（只读状态元数据；无像素/读回/新计时器；证据边界：Stats A/B 括读只证明身份稳定，非全体原子同步；StatsB 为测速基准）"
                + BuildSharedDetail(snap, rate);
        }

        // Plain-language classification per the frozen design (SHORT labels for the
        // ~500px overlay; liveReject/Reason evidence stays in details). Invalid/
        // read-error snapshots are now intercepted in OnTick BEFORE this branch —
        // the checks below remain defensive only. LiveValid false NEVER shows green.
        private static string ClassifySharedState(VRSIMPlugin.SharedGpuMonitorSample snap, SharedRateResult rate)
        {
            if (snap == null) return "读取失败";
            if (!snap.Valid || snap.ReadError) return "读取失败/失效";
            if (snap.SharedPreparing) return "准备中";
            if (IsStoppedPhase(snap.Phase)) return "已停止";
            if (snap.BoundValid && snap.LiveValid)
            {
                if (rate == null) return "采样中";
                if (rate.Stalled) return "更新停滞";
                if (rate.Seeding) return "采样中";
                return "内容更新中";
            }
            return "准备中";
        }

        private string BuildSharedDetail(VRSIMPlugin.SharedGpuMonitorSample snap, SharedRateResult rate)
        {
            var ci = CultureInfo.InvariantCulture;
            var sb = new StringBuilder();
            sb.AppendLine();
            sb.Append("flags: valid=").Append(B(snap.Valid)).Append(" readError=").Append(B(snap.ReadError))
              .Append(" sharedSelected=").Append(B(snap.SharedSelected))
              .Append(" preparing=").Append(B(snap.SharedPreparing))
              .Append(" bound=").Append(B(snap.BoundValid)).Append(" live=").Append(B(snap.LiveValid)).AppendLine();
            sb.Append("reason=").Append(OrUnkn(snap.Reason))
              .Append(" | liveReject=").Append(OrDash(snap.LiveReject))
              .Append(" | phase=").Append(OrUnkn(snap.Phase))
              .Append(" | failure=").Append(OrDash(EmptyToNull(snap.Failure))).AppendLine();
            sb.Append("identity(仅引用相等键，不读成员): producerRef=")
              .Append(snap.ProducerIdentity != null ? "(held)" : "(none)")
              .Append(" mainGen=").Append(snap.MainGeneration.ToString(ci))
              .Append(" flowGen=").Append(snap.ActiveFlowGeneration.ToString(ci))
              .Append(" hostedStartGen=").Append(snap.OriginalHostedStartGeneration.ToString(ci)).AppendLine();
            sb.Append("compiled: edge=").Append(snap.CompiledRequestedEdge.ToString(ci))
              .Append(" frame=").Append(snap.CompiledFrameWidth.ToString(ci)).Append('x').Append(snap.CompiledFrameHeight.ToString(ci))
              .Append(" | actual: ").Append(snap.ActualWidth.HasValue && snap.ActualHeight.HasValue
                  ? snap.ActualWidth.Value.ToString(ci) + "×" + snap.ActualHeight.Value.ToString(ci) + "（来自已验证 StatsB）"
                  : "未确认（仅按配置门校验）").AppendLine();
            sb.Append("nativeState=").Append(snap.ActualNativeState.ToString(ci))
              .Append('(').Append(VRSIMPlugin.StateName(snap.ActualNativeState)).Append(')').AppendLine();
            if (snap.HasFrameTiming)
            {
                double hz = snap.DisplayPeriodNs > 0 ? 1e9 / snap.DisplayPeriodNs : double.NaN;
                sb.Append("frameTiming: periodNs=").Append(snap.DisplayPeriodNs.ToString(ci))
                  .Append(" → XR报告刷新率=").Append(double.IsNaN(hz) ? "n/a" : hz.ToString("0.0", ci) + "Hz（非内容fps）")
                  .Append(" trackingValid=").Append(snap.TrackingValid.ToString(ci))
                  .Append(" shouldRender=").Append(snap.ShouldRender.ToString(ci))
                  .Append(" timingFrameSerial=").Append(snap.TimingFrameSerial.ToString(ci)).AppendLine();
            }
            else sb.Append("frameTiming: 快照内不可用（不冒充）").AppendLine();

            if (snap.StatsA.HasValue)
            {
                var a = snap.StatsA.Value;
                sb.Append("StatsA(括读参考，非测速基准): epoch=").Append(a.sessionEpoch.ToString(ci))
                  .Append(" uniqueLiveLayerOk=").Append(a.uniqueLiveLayerOk.ToString(ci)).AppendLine();
            }
            else sb.Append("StatsA: 无").AppendLine();
            if (snap.StatsB.HasValue)
            {
                var b = snap.StatsB.Value;
                sb.Append("StatsB(测速基准): epoch=").Append(b.sessionEpoch.ToString(ci))
                  .Append(" uniqueLiveLayerOk=").Append(b.uniqueLiveLayerOk.ToString(ci))
                  .Append(" activeSource=").Append(b.activeSource.ToString(ci))
                  .Append(" active=").Append(b.activeWidth.ToString(ci)).Append('x').Append(b.activeHeight.ToString(ci))
                  .Append(" activeFrame/Pose=").Append(b.activeFrameSerial.ToString(ci)).Append('/').Append(b.activePoseSerial.ToString(ci))
                  .Append(" lastSuccessFrame/Pose=").Append(b.lastSuccessfulLayerFrameSerial.ToString(ci)).Append('/')
                  .Append(b.lastSuccessfulLayerPoseSerial.ToString(ci))
                  .Append(" reserved=").Append(b.reserved.ToString(ci)).AppendLine();
                sb.Append("StatsB非速率计数(累计，绝不可当FPS): rawAccepted=").Append(b.rawAccepted.ToString(ci))
                  .Append(" rawRejected=").Append(b.rawRejected.ToString(ci))
                  .Append(" gpuPairCommitted=").Append(b.gpuPairCommitted.ToString(ci))
                  .Append(" gpuPairFailed=").Append(b.gpuPairFailed.ToString(ci))
                  .Append(" exactPoseRejected=").Append(b.exactPoseRejected.ToString(ci)).AppendLine();
            }
            else sb.Append("StatsB: 无（本轮不测速）").AppendLine();

            if (snap.Stop.HasValue)
            {
                var sp = snap.Stop.Value;
                sb.Append("Stop32: rc=").Append(snap.StopRc.ToString(ci))
                  .Append(" flags=").Append(sp.flags.ToString(ci))
                  .Append(" pendingSlots=").Append(sp.pendingSlots.ToString(ci))
                  .Append(" boundEpoch=").Append(sp.boundEpoch.ToString(ci))
                  .Append(" boundGeneration=").Append(sp.boundGeneration.ToString(ci)).AppendLine();
            }
            else sb.Append("Stop32: 无（rc=").Append(snap.StopRc.ToString(ci)).Append("）").AppendLine();
            if (snap.Bridge.HasValue)
            {
                var br = snap.Bridge.Value;
                sb.Append("Bridge72: rc=").Append(snap.BridgeRc.ToString(ci))
                  .Append(" init=").Append(br.initCount.ToString(ci))
                  .Append(" prepare=").Append(br.prepareCount.ToString(ci))
                  .Append(" commit=").Append(br.commitCount.ToString(ci))
                  .Append(" drop=").Append(br.dropCount.ToString(ci))
                  .Append(" drain=").Append(br.drainCount.ToString(ci))
                  .Append(" quarantine=").Append(br.quarantineCount.ToString(ci))
                  .Append(" slots=").Append(br.slotState0.ToString(ci)).Append('/').Append(br.slotState1.ToString(ci))
                  .Append('/').Append(br.slotState2.ToString(ci)).AppendLine();
            }
            else sb.Append("Bridge72: 无（rc=").Append(snap.BridgeRc.ToString(ci)).Append("）").AppendLine();

            sb.Append("速率证据: sampleQpc=").Append(snap.SampleQpc.ToString(ci))
              .Append(" qpcFrequency=").Append(Stopwatch.Frequency.ToString(ci)).AppendLine();
            if (rate != null)
            {
                sb.Append("rate: eligible=").Append(B(rate.Eligible))
                  .Append(" seeding=").Append(B(rate.Seeding))
                  .Append(" stalled=").Append(B(rate.Stalled))
                  .Append(" rateKnown=").Append(B(rate.RateKnown));
                if (rate.RateKnown) sb.Append(" rate=").Append(rate.RateFps.ToString("0.0", ci)).Append("fps");
                if (rate.AgeKnown && rate.Eligible)
                    sb.Append(_sharedRate.HasLastObservedNewQpc ? " 新内容观察距今≈" : " 首次采样至今≈")
                      .Append(rate.AgeMs.ToString("0", ci)).Append("ms（轮询粒度约300ms，非端到端时延）");
                if (rate.HistoryKnown)
                    sb.Append(" | 历史已测正速率=").Append(rate.HistoryFps.ToString("0.0", ci))
                      .Append("fps（历史事实，非当前live速率）");
                if (rate.LastWindowKnown)
                    sb.Append(" | 最近窗口: delta=").Append(rate.LastWindowDelta.ToString(ci))
                      .Append(" span≈").Append(rate.LastWindowSpanMs.ToString("0", ci)).Append("ms")
                      .Append(" rate=").Append(rate.LastWindowRateFps.ToString("0.0", ci)).Append("fps")
                      .Append(rate.LastWindowZeroDelta ? "（实测0：窗口内无新交付，非CPU预览缺失失败）" : "");
                sb.Append(" | uniqueCount=").Append(rate.ObservedUniqueCount.ToString(ci));
                sb.Append(" | note=").Append(OrUnkn(rate.Note)).AppendLine();
            }
            else sb.Append("rate: 本轮计算异常，锚点已复位").AppendLine();
            sb.Append("边界: 该速率=真实不同场景帧成功交付（uniqueLiveLayerOk），非 submit/prepare/commit 速率、非像素变动、非合成呈现/ASW、非用户感知FPS；XR Hz 来自 runtime 时序报告；")
              .Append("UI 轮询约300ms、120 调度目标均不是帧率，年龄以监控观察 tick 粒度计（非采集/呈现端到端延迟）；")
              .Append("已完成零 delta 窗口如实记 0（非因 CPU 预览缺失而判失败）。");
            return sb.ToString();
        }

        private static string B(bool v) { return v ? "Y" : "N"; }

        private static string EmptyToNull(string s) { return string.IsNullOrEmpty(s) ? null : s; }

        private static string OrUnkn(string s) { return string.IsNullOrEmpty(s) ? "?" : s; }

        private static string OrDash(string s) { return string.IsNullOrEmpty(s) ? "-" : s; }

        private static bool EyeViewSane(VRSIMPlugin.VRSIMEyeView v)
        {
            if (v.posMm == null || v.quatXyzw == null || v.posMm.Length != 3 || v.quatXyzw.Length != 4) return false;
            foreach (double d in v.posMm) if (double.IsNaN(d) || double.IsInfinity(d)) return false;
            foreach (double d in v.quatXyzw) if (double.IsNaN(d) || double.IsInfinity(d)) return false;
            double n = Math.Sqrt(v.quatXyzw[0] * v.quatXyzw[0] + v.quatXyzw[1] * v.quatXyzw[1] +
                                 v.quatXyzw[2] * v.quatXyzw[2] + v.quatXyzw[3] * v.quatXyzw[3]);
            return n > 0.9 && n < 1.1;
        }

        private static string Deg(double rad)
        { return (rad * 180.0 / Math.PI).ToString("0.0", CultureInfo.InvariantCulture); }

        private static ImageSource FreezeIt(BitmapSource bs)
        { if (bs.CanFreeze) bs.Freeze(); return bs; }

        private static void AppendErr(StringBuilder errs, string what, Exception ex)
        { errs.Append(what).Append(':').Append(ex.GetType().Name).Append(':').Append(ex.Message).Append("; "); }
    }
}