using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace Hyperlink
{
    sealed class PrivacyScreen : IDisposable
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct Version { public int Size, Major, Minor, Build, Platform; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string ServicePack; }
        [DllImport("ntdll.dll", CharSet = CharSet.Unicode)] static extern int RtlGetVersion(ref Version version);
        [DllImport("dwmapi.dll")] static extern int DwmIsCompositionEnabled([MarshalAs(UnmanagedType.Bool)] out bool enabled);
        [DllImport("user32.dll", SetLastError = true)] static extern bool SetWindowDisplayAffinity(IntPtr window, uint affinity);
        [DllImport("user32.dll", SetLastError = true)] static extern bool SetLayeredWindowAttributes(IntPtr window, uint color, byte alpha, uint flags);
        [DllImport("user32.dll", SetLastError = true)] static extern bool RegisterHotKey(IntPtr window, int id, uint modifiers, uint key);
        [DllImport("user32.dll")] static extern bool UnregisterHotKey(IntPtr window, int id);
        [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr first, IntPtr second);
        sealed class Cover : Form
        {
            public Label Notice;
            protected override bool ShowWithoutActivation { get { return true; } }
            protected override CreateParams CreateParams { get { var value = base.CreateParams; value.ExStyle |= 0x08080020; return value; } }
            public Cover(Rectangle bounds)
            {
                FormBorderStyle = FormBorderStyle.None; StartPosition = FormStartPosition.Manual; Bounds = bounds;
                TopMost = true; ShowInTaskbar = false; BackColor = Color.Black;
                Notice = new Label { Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter, ForeColor = Color.White, BackColor = Color.Black, Font = Theme.Font(16) }; Controls.Add(Notice);
            }
        }
        sealed class EscapeWindow : NativeWindow, IDisposable
        {
            readonly Action escape;
            public EscapeWindow(Action action)
            {
                escape = action; CreateHandle(new CreateParams { Caption = "Hyperlink privacy recovery", Parent = new IntPtr(-3) });
                if (!RegisterHotKey(Handle, 1, 0x4000 | 1 | 2 | 4, 0x48)) { DestroyHandle(); throw new InvalidOperationException("The privacy recovery hotkey Ctrl+Alt+Shift+H is unavailable."); }
            }
            protected override void WndProc(ref Message message) { if (message.Msg == 0x0312) escape(); base.WndProc(ref message); }
            public void Dispose() { if (Handle != IntPtr.Zero) { UnregisterHotKey(Handle, 1); DestroyHandle(); } }
        }
        readonly Action disconnect;
        readonly TaskCompletionSource<bool> ready = new TaskCompletionSource<bool>();
        readonly TaskCompletionSource<bool> ended = new TaskCompletionSource<bool>();
        readonly Rectangle[] bounds;
        readonly List<Cover> covers = new List<Cover>();
        Control dispatcher; ApplicationContext context; int disposed; volatile bool recording;
        IntPtr escapeHandle;
        internal Task Completion { get { return ended.Task; } }
        internal bool Active { get { return Volatile.Read(ref disposed) == 0; } }
        internal PrivacyScreen(Action endSession, Rectangle[] testBounds = null)
        {
            if (endSession == null) throw new InvalidOperationException("Privacy recovery is unavailable.");
            disconnect = endSession;
            bounds = testBounds ?? Array.ConvertAll(Screen.AllScreens, screen => screen.Bounds);
            var version = new Version { Size = Marshal.SizeOf(typeof(Version)) }; bool composition;
            if (RtlGetVersion(ref version) != 0 || version.Major < 10 || version.Build < 19041 || DwmIsCompositionEnabled(out composition) != 0 || !composition) throw new InvalidOperationException("Privacy mode requires Windows 10 version 2004 or later with desktop composition.");
            var thread = new Thread(Run) { IsBackground = true, Name = "Hyperlink privacy recovery" }; thread.SetApartmentState(ApartmentState.STA); thread.Start();
            try { if (!ready.Task.Wait(5000)) throw new TimeoutException("Privacy mode did not start in time."); }
            catch (AggregateException ex) { Dispose(); throw ex.InnerException; }
            catch { Dispose(); throw; }
        }
        void Run()
        {
            EventHandler displayChanged = delegate { Post(Panic); };
            SessionSwitchEventHandler sessionChanged = delegate(object sender, SessionSwitchEventArgs args) { if (args.Reason == SessionSwitchReason.SessionLock || args.Reason == SessionSwitchReason.SessionLogoff || args.Reason == SessionSwitchReason.ConsoleDisconnect || args.Reason == SessionSwitchReason.RemoteDisconnect) Post(Panic); };
            try
            {
                context = new ApplicationContext(); dispatcher = new Control(); dispatcher.CreateControl(); dispatcher.Handle.ToInt64();
                SystemEvents.DisplaySettingsChanged += displayChanged; SystemEvents.SessionSwitch += sessionChanged;
                using (var escape = new EscapeWindow(Panic))
                using (var watchdog = new System.Windows.Forms.Timer { Interval = 30 * 60 * 1000 })
                {
                    escapeHandle = escape.Handle;
                    foreach (Rectangle area in bounds)
                    {
                        var cover = new Cover(area); covers.Add(cover);
                        if (!SetLayeredWindowAttributes(cover.Handle, 0, 255, 2) || !SetWindowDisplayAffinity(cover.Handle, 0x11)) throw new InvalidOperationException("Windows could not prepare the privacy cover for input and capture.");
                    }
                    if (!Active) throw new OperationCanceledException();
                    UpdateNotice(); foreach (Cover cover in covers) cover.Show();
                    watchdog.Tick += delegate { Panic(); }; watchdog.Start(); ready.TrySetResult(true);
                    Application.Run(context);
                }
            }
            catch (Exception ex) { if (!ready.TrySetException(ex)) ThreadPool.QueueUserWorkItem(delegate { try { disconnect(); } catch { } }); }
            finally
            {
                Interlocked.Exchange(ref disposed, 1);
                SystemEvents.DisplaySettingsChanged -= displayChanged; SystemEvents.SessionSwitch -= sessionChanged;
                foreach (Cover cover in covers) cover.Dispose(); if (dispatcher != null) dispatcher.Dispose(); if (context != null) context.Dispose();
                ended.TrySetResult(true);
            }
        }
        internal void SetRecording(bool value)
        {
            recording = value;
            Post(UpdateNotice);
        }
        void UpdateNotice()
        {
            string text = "Hyperlink privacy mode" + (recording ? " · RECORDING" : "") + "\r\n\r\nCtrl + Alt + Shift + H\r\nRestores the display and disconnects the viewer\r\n\r\nAutomatically stops after 30 minutes";
            foreach (Cover cover in covers) cover.Notice.Text = text;
        }
        void Post(Action action)
        {
            var control = dispatcher;
            if (control != null && !control.IsDisposed) try { control.BeginInvoke(action); } catch (InvalidOperationException) { }
        }
        void CloseCovers()
        {
            foreach (Cover cover in covers) cover.Hide(); if (context != null) context.ExitThread();
        }
        void Panic()
        {
            if (Interlocked.Exchange(ref disposed, 1) != 0) return;
            CloseCovers(); ThreadPool.QueueUserWorkItem(delegate { try { disconnect(); } catch { } });
        }
        public void Dispose() { if (Interlocked.Exchange(ref disposed, 1) == 0) Post(CloseCovers); }
        internal void TestEmergencyMessage() { if (!PostMessage(escapeHandle, 0x0312, new IntPtr(1), new IntPtr((0x48 << 16) | 7))) throw new InvalidOperationException("Emergency message could not be delivered."); }
    }
}
