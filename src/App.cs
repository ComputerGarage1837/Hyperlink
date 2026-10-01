using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace Hyperlink
{
    static class Theme
    {
        public static readonly Color Background = Color.FromArgb(12, 17, 25), Sidebar = Color.FromArgb(17, 23, 33), Card = Color.FromArgb(22, 30, 42), Border = Color.FromArgb(43, 55, 71);
        public static readonly Color Text = Color.FromArgb(235, 242, 250), Muted = Color.FromArgb(153, 169, 190), Accent = Color.FromArgb(107, 231, 196), Red = Color.FromArgb(255, 142, 142);
        public static Font Font(float size, FontStyle style = FontStyle.Regular) { return new Font("Segoe UI", size, style); }
        public static Label Label(string text, float size = 10, Color? color = null, FontStyle style = FontStyle.Regular)
        { return new Label { Text = text, ForeColor = color ?? Text, Font = Font(size, style), AutoSize = false, Dock = DockStyle.Top, Height = (int)(size * 2.8), BackColor = Color.Transparent }; }
        public static Button Button(string text, bool primary = false)
        {
            var b = new Button { Text = text, FlatStyle = FlatStyle.Flat, Height = 42, Cursor = Cursors.Hand, Font = Font(10, FontStyle.Bold), BackColor = primary ? Accent : Color.FromArgb(33, 45, 59), ForeColor = primary ? Background : Text, AutoSize = false, Margin = new Padding(0, 8, 8, 0) };
            b.FlatAppearance.BorderSize = primary ? 0 : 1; b.FlatAppearance.BorderColor = Border;
            b.FlatAppearance.MouseOverBackColor = primary ? Color.FromArgb(142, 242, 214) : Color.FromArgb(47, 63, 81);
            b.FlatAppearance.MouseDownBackColor = primary ? Color.FromArgb(86, 200, 168) : Border;
            return b;
        }
        public static TextBox Field(string text)
        { return new TextBox { Text = text, BackColor = Color.FromArgb(14, 21, 31), ForeColor = Text, BorderStyle = BorderStyle.FixedSingle, Font = Font(11), Height = 32, Dock = DockStyle.Top }; }
        public static void FitLabels(TableLayoutPanel panel)
        { foreach (Control c in panel.Controls) if (c is Label) c.Dock = DockStyle.Fill; }
        public static void Error(IWin32Window owner, Exception ex)
        { MessageBox.Show(owner, ex.Message, "Hyperlink", MessageBoxButtons.OK, MessageBoxIcon.Information); }
    }

    sealed class Card : Panel
    {
        public Card() { BackColor = Theme.Card; Padding = new Padding(20); DoubleBuffered = true; }
        protected override void OnPaint(PaintEventArgs e)
        { base.OnPaint(e); using (var p = new Pen(Theme.Border)) e.Graphics.DrawRectangle(p, 0, 0, Width - 1, Height - 1); }
    }
    sealed class Brand : Control
    {
        public Brand() { Height = 88; Dock = DockStyle.Top; DoubleBuffered = true; }
        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = new Pen(Theme.Accent, 4))
            {
                p.StartCap = LineCap.Round; p.EndCap = LineCap.Round;
                e.Graphics.DrawArc(p, 22, 27, 24, 30, 55, 250);
                e.Graphics.DrawArc(p, 38, 27, 24, 30, 235, 250);
                e.Graphics.DrawLine(p, 39, 42, 47, 42);
            }
            using (var f = Theme.Font(16, FontStyle.Bold)) using (var b = new SolidBrush(Theme.Text)) e.Graphics.DrawString("Hyperlink", f, b, 77, 24);
            using (var f = Theme.Font(8)) using (var b = new SolidBrush(Theme.Muted)) e.Graphics.DrawString("PRIVATE REMOTE DESKTOP", f, b, 24, 69);
        }
    }

    sealed class Approval : Form
    {
        public int Permission;
        public Approval(string name, bool pairing)
        {
            Text = "Hyperlink · Local approval"; ClientSize = new Size(560, 360); StartPosition = FormStartPosition.CenterParent;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false; BackColor = Theme.Background; ForeColor = Theme.Text; TopMost = true;
            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(25), ColumnCount = 1, RowCount = 4 };
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 64)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 70)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 45));
            layout.Controls.Add(Theme.Label(pairing ? "Pair a trusted computer" : "Allow this connection?", 16, null, FontStyle.Bold), 0, 0);
            layout.Controls.Add(Theme.Label(name + (pairing ? " wants permission to connect to this computer." : " wants to view this computer now."), 11), 0, 1);
            layout.Controls.Add(Theme.Label(pairing ? "Only approve an invitation you shared. Every session still requires your approval. You can revoke access at any time." : "Screen content becomes visible after approval. Control also permits pointer and keyboard input. Stop the session locally at any time.", 10, Theme.Muted), 0, 2);
            var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
            foreach (var option in new[] { new { Text = "Decline", Value = 0 }, new { Text = "View only", Value = 1 }, new { Text = "View + control", Value = 2 } })
            { int value = option.Value; var b = Theme.Button(option.Text, value == 2); b.Width = 155; b.Margin = new Padding(0, 0, 8, 0); b.Click += delegate { Permission = value; DialogResult = DialogResult.OK; }; buttons.Controls.Add(b); }
            layout.Controls.Add(buttons, 0, 3); Theme.FitLabels(layout); Controls.Add(layout);
        }
    }
    sealed class PairDialog : Form
    {
        readonly Store store;
        readonly TextBox invitation;
        readonly Label hint;
        readonly Button pair;
        public PairDialog(Store state)
        {
            store = state; Text = "Hyperlink · Add a computer"; ClientSize = new Size(590, 380); BackColor = Theme.Background; ForeColor = Theme.Text;
            StartPosition = FormStartPosition.CenterParent; FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(25), ColumnCount = 1, RowCount = 5 };
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 45)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 75)); layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 40)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 47));
            layout.Controls.Add(Theme.Label("Bring a computer into reach", 18, null, FontStyle.Bold), 0, 0);
            layout.Controls.Add(Theme.Label("On the other computer, open Hyperlink, start hosting, and create an invitation. Paste it below. Its owner must approve pairing.", 11, Theme.Muted), 0, 1);
            invitation = Theme.Field(""); invitation.Multiline = true; invitation.Dock = DockStyle.Fill; invitation.ScrollBars = ScrollBars.Vertical; invitation.MaxLength = 4096; layout.Controls.Add(invitation, 0, 2);
            hint = Theme.Label("Invitations expire after five minutes and work once.", 9, Theme.Muted); hint.Padding = new Padding(0, 10, 0, 0); layout.Controls.Add(hint, 0, 3);
            pair = Theme.Button("Request pairing", true); pair.Dock = DockStyle.Fill; pair.Click += Pair; layout.Controls.Add(pair, 0, 4); Theme.FitLabels(layout); Controls.Add(layout);
        }
        void Pair(object sender, EventArgs args)
        {
            Invitation decoded;
            try { decoded = Invitation.Decode(invitation.Text); } catch (Exception ex) { Theme.Error(this, ex); return; }
            pair.Enabled = false; invitation.Enabled = false; hint.Text = "Connecting securely. Waiting for the host's local approval…";
            ThreadPool.QueueUserWorkItem(delegate
            {
                Exception error = null;
                try { using (var remote = new Remote(store)) remote.Pair(decoded); } catch (Exception ex) { error = ex; }
                if (IsDisposed || Disposing) return;
                try { BeginInvoke((Action)delegate { if (error == null) { DialogResult = DialogResult.OK; Close(); } else { pair.Enabled = true; invitation.Enabled = true; hint.Text = "Pairing did not complete."; Theme.Error(this, error); } }); } catch (InvalidOperationException) { }
            });
        }
    }

    sealed class Surface : Control
    {
        public Remote Remote;
        readonly object sync = new object();
        Bitmap current, pending;
        readonly HashSet<MouseButtons> heldButtons = new HashSet<MouseButtons>();
        bool scheduled;
        long sequence, painted;
        internal bool HasPresentedFrame { get { lock (sync) return painted > 0; } }
        int presented;
        public int TakePresented() { return Interlocked.Exchange(ref presented, 0); }
        public Surface() { DoubleBuffered = true; BackColor = Color.Black; Dock = DockStyle.Fill; TabStop = true; SetStyle(ControlStyles.Selectable, true); }
        public void Receive(Bitmap image)
        {
            lock (sync)
            {
                if (IsDisposed || Disposing || !IsHandleCreated) { image.Dispose(); return; }
                if (pending != null) pending.Dispose(); pending = image;
                if (scheduled) return; scheduled = true;
            }
            try
            {
                BeginInvoke((Action)delegate
                {
                    lock (sync)
                    {
                        scheduled = false;
                        if (pending == null) return;
                        if (current != null) current.Dispose(); current = pending; pending = null; sequence++;
                    }
                    Invalidate();
                });
            }
            catch (InvalidOperationException) { lock (sync) { scheduled = false; if (pending != null) pending.Dispose(); pending = null; } }
        }
        Rectangle Area()
        {
            if (Remote == null || Remote.Width == 0 || Remote.Height == 0) return Rectangle.Empty;
            double scale = Math.Min(Width / (double)Remote.Width, Height / (double)Remote.Height);
            int w = (int)(Remote.Width * scale), h = (int)(Remote.Height * scale);
            return new Rectangle((Width - w) / 2, (Height - h) / 2, w, h);
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            lock (sync)
            {
                if (current != null)
                {
                    e.Graphics.InterpolationMode = InterpolationMode.Bilinear; e.Graphics.DrawImage(current, Area());
                    if (painted != sequence) { painted = sequence; Interlocked.Increment(ref presented); }
                }
                else using (var f = Theme.Font(12)) using (var b = new SolidBrush(Theme.Muted)) e.Graphics.DrawString("Waiting for an approved connection…", f, b, 28, 28);
            }
        }
        bool Pointer(Point point)
        {
            Rectangle a = Area(); if (a.Width < 1 || a.Height < 1 || !a.Contains(point) || Remote == null) return false;
            Remote.Send(new { kind = "input", type = "move", x = (point.X - a.X) * Remote.Width / a.Width, y = (point.Y - a.Y) * Remote.Height / a.Height }); return true;
        }
        long lastMove;
        bool normalCaptureRelease;
        protected override void OnMouseMove(MouseEventArgs e)
        { base.OnMouseMove(e); long now = Stopwatch.GetTimestamp(); if ((now - lastMove) * 1000 / Stopwatch.Frequency < 12) return; lastMove = now; Pointer(e.Location); }
        protected override void OnMouseDown(MouseEventArgs e)
        { base.OnMouseDown(e); Focus(); if (Pointer(e.Location)) { heldButtons.Add(e.Button); Capture = true; Button(e.Button, true); } }
        protected override void OnMouseUp(MouseEventArgs e)
        { base.OnMouseUp(e); Pointer(e.Location); heldButtons.Remove(e.Button); Button(e.Button, false); normalCaptureRelease = true; try { Capture = heldButtons.Count > 0; } finally { normalCaptureRelease = false; } }
        protected override void OnMouseCaptureChanged(EventArgs e) { base.OnMouseCaptureChanged(e); if (!Capture && !normalCaptureRelease) Release(); }
        void Button(MouseButtons button, bool down)
        { if (Remote != null && (button == MouseButtons.Left || button == MouseButtons.Right || button == MouseButtons.Middle)) Remote.Send(new { kind = "input", type = "button", button = button == MouseButtons.Left ? 0 : button == MouseButtons.Right ? 1 : 2, down = down ? 1 : 0 }); }
        protected override void OnMouseWheel(MouseEventArgs e)
        { base.OnMouseWheel(e); if (Remote != null) Remote.Send(new { kind = "input", type = "wheel", delta = Math.Max(-1200, Math.Min(1200, e.Delta)) }); }
        protected override bool IsInputKey(Keys keyData) { return true; }
        protected override bool ProcessDialogKey(Keys keyData) { return false; }
        protected override void OnKeyDown(KeyEventArgs e)
        { if (Remote != null) Remote.Send(new { kind = "input", type = "key", key = e.KeyValue, down = 1 }); e.Handled = true; base.OnKeyDown(e); }
        protected override void OnKeyUp(KeyEventArgs e)
        { if (Remote != null) Remote.Send(new { kind = "input", type = "key", key = e.KeyValue, down = 0 }); e.Handled = true; base.OnKeyUp(e); }
        public void Release() { heldButtons.Clear(); normalCaptureRelease = true; try { Capture = false; } finally { normalCaptureRelease = false; } if (Remote != null) Remote.Send(new { kind = "input", type = "release" }); }
        protected override void OnLostFocus(EventArgs e) { Release(); base.OnLostFocus(e); }
        protected override void Dispose(bool disposing)
        { if (disposing) { Release(); lock (sync) { if (pending != null) pending.Dispose(); pending = null; if (current != null) current.Dispose(); current = null; } } base.Dispose(disposing); }
    }

    sealed class Viewer : Form
    {
        readonly Remote remote;
        readonly Surface surface;
        readonly Label state;
        readonly System.Windows.Forms.Timer meter;
        bool fullscreen;
        Rectangle savedBounds;
        internal bool HasPresentedFrame { get { return surface.HasPresentedFrame; } }
        internal string ConnectionState { get { return state.Text; } }
        public Viewer(Store store, Device device)
        {
            Text = device.Name + " · Hyperlink"; Size = new Size(1080, 760); MinimumSize = new Size(980, 600); StartPosition = FormStartPosition.CenterScreen;
            Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            BackColor = Theme.Background; ForeColor = Theme.Text;
            var bar = new TableLayoutPanel { Dock = DockStyle.Top, Height = 84, Padding = new Padding(16), BackColor = Theme.Sidebar, ColumnCount = 5, RowCount = 1 };
            bar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); for (int i = 0; i < 4; i++) bar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 155));
            var disconnect = Theme.Button("Disconnect"); disconnect.Dock = DockStyle.Fill; disconnect.Click += delegate { Close(); }; bar.Controls.Add(disconnect, 3, 0);
            var transfers = Theme.Button("Session tools"); transfers.Dock = DockStyle.Fill; transfers.Click += delegate { if (!remote.SupportsExtensions) { MessageBox.Show(this, "Update the host to use files and clipboard."); return; } TransferPanel.ShowFor(remote, this); }; bar.Controls.Add(transfers, 4, 0);
            var full = Theme.Button("Full screen"); full.Dock = DockStyle.Fill; full.Click += delegate { if (!fullscreen) { savedBounds = Bounds; FormBorderStyle = FormBorderStyle.None; WindowState = FormWindowState.Normal; Bounds = Screen.FromControl(this).Bounds; full.Text = "Exit full screen"; } else { FormBorderStyle = FormBorderStyle.Sizable; Bounds = savedBounds; full.Text = "Full screen"; } fullscreen = !fullscreen; }; bar.Controls.Add(full, 2, 0);
            var release = Theme.Button("Release keys"); release.Dock = DockStyle.Fill; release.Click += delegate { surface.Release(); }; bar.Controls.Add(release, 1, 0);
            state = Theme.Label("Connecting securely…", 9, Theme.Muted); state.Dock = DockStyle.Fill; state.TextAlign = ContentAlignment.MiddleLeft; bar.Controls.Add(state, 0, 0);
            surface = new Surface(); Controls.Add(surface); Controls.Add(bar);
            remote = new Remote(store); surface.Remote = remote; remote.Frame = surface.Receive;
            remote.Ended = delegate(string message) { if (!IsDisposed) try { BeginInvoke((Action)delegate { state.Text = message; state.ForeColor = Theme.Red; }); } catch (InvalidOperationException) { } };
            meter = new System.Windows.Forms.Timer { Interval = 1000 }; meter.Tick += delegate { int fps = surface.TakePresented(); if (remote.Connected) state.Text = (remote.Control ? "VIEW + CONTROL" : "VIEW ONLY") + "   ·   " + fps + " presented fps\nTLS 1.2 · " + remote.Width + " × " + remote.Height + " · Draft cap: 30 fps"; }; meter.Start();
            Shown += delegate
            {
                // Decoder callbacks must find an HWND created on the UI thread, even in hidden UI checks.
                surface.Handle.ToInt64();
                ThreadPool.QueueUserWorkItem(delegate
                {
                    try { remote.Connect(device); if (!IsDisposed) BeginInvoke((Action)delegate { state.Text = "Connected · Click the picture to focus remote input."; surface.Focus(); }); }
                    catch (Exception ex) { if (!IsDisposed) try { BeginInvoke((Action)delegate { state.Text = "Connection did not complete"; state.ForeColor = Theme.Red; Theme.Error(this, ex); }); } catch (InvalidOperationException) { } }
                });
            };
            FormClosing += delegate { surface.Release(); remote.Dispose(); meter.Stop(); meter.Dispose(); };
        }
    }

    sealed class MainWindow : Form
    {
        internal bool CanInstallAutomatically { get { return Application.OpenForms.Count == 1 && !host.SessionActive; } }
        internal bool InstallAutomatically(string job) { if (!CanInstallAutomatically || !host.BeginAutomaticUpdate(delegate { UpdateCoordinator.Start(job); })) return false; Close(); return true; }
        readonly Store store;
        readonly Host host;
        readonly AccountClient account;
        readonly Panel content;
        readonly Label eventLabel;
        readonly Dictionary<string, Button> navigation = new Dictionary<string, Button>();
        Label hostState;
        string page = "Computers", latest = "Ready. Hosting is off until you enable it.";
        bool busy;
        readonly System.Windows.Forms.Timer refresh;
        public MainWindow(Store state)
        {
            store = state; account = new AccountClient(store.Key, "https://hyperlink.myfamilyapps.ca"); Text = "Hyperlink · Private remote desktop"; Size = new Size(1240, 830); MinimumSize = new Size(1100, 760);
            Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            StartPosition = FormStartPosition.CenterScreen; BackColor = Theme.Background; ForeColor = Theme.Text; Font = Theme.Font(10); AutoScaleMode = AutoScaleMode.Dpi;
            host = new Host(store, Approve, Changed);
            var sidebar = new Panel { Dock = DockStyle.Left, Width = 270, BackColor = Theme.Sidebar, Padding = new Padding(16, 4, 16, 18) };
            var nav = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(0, 25, 0, 0) };
            foreach (string name in new[] { "Computers", "This computer", "Access", "Family account", "Wake a computer", "About this draft" })
            { string target = name; var b = Theme.Button(name); b.Width = 238; b.Height = 48; b.TextAlign = ContentAlignment.MiddleLeft; b.Padding = new Padding(12, 0, 0, 0); b.Margin = new Padding(0, 0, 0, 10); b.Click += delegate { page = target; Render(); }; navigation.Add(name, b); nav.Controls.Add(b); }
            var bottom = new Panel { Dock = DockStyle.Bottom, Height = 170 };
            var identity = Theme.Label("LOCAL IDENTITY\n" + store.Data.Name + "\n\nWindows-protected\nlocal identity", 9, Theme.Muted); identity.Dock = DockStyle.Fill; bottom.Controls.Add(identity);
            var version = Theme.Label("v0.5.6   /   WINDOWS DRAFT", 8, Theme.Accent); version.Dock = DockStyle.Bottom; version.Height = 25; bottom.Controls.Add(version);
            sidebar.Controls.Add(nav); sidebar.Controls.Add(bottom); sidebar.Controls.Add(new Brand());
            var shell = new Panel { Dock = DockStyle.Fill, Padding = new Padding(30, 22, 30, 18) };
            content = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
            var foot = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = 88, Padding = new Padding(0, 15, 0, 0), ColumnCount = 2, RowCount = 1 };
            foot.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); foot.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 180));
            var stop = Theme.Button("STOP SESSION"); stop.ForeColor = Theme.Red; stop.Dock = DockStyle.Fill; stop.Click += delegate { host.StopSession(); }; foot.Controls.Add(stop, 1, 0);
            eventLabel = Theme.Label(latest, 9, Theme.Muted); eventLabel.Dock = DockStyle.Fill; eventLabel.TextAlign = ContentAlignment.MiddleLeft; foot.Controls.Add(eventLabel, 0, 0);
            shell.Controls.Add(content); shell.Controls.Add(foot); Controls.Add(shell); Controls.Add(sidebar);
            refresh = new System.Windows.Forms.Timer { Interval = 1000 }; refresh.Tick += delegate { if (hostState != null && !hostState.IsDisposed) hostState.Text = host.Active ? "●  Someone is connected" : host.Running ? "●  Hosting is on" : "○  Hosting is off"; }; refresh.Start();
            FormClosing += delegate { refresh.Stop(); refresh.Dispose(); host.Dispose(); };
            Render();
        }
        int Approve(string name, bool pairing)
        {
            if (IsDisposed || Disposing || !IsHandleCreated) return 0;
            try
            {
                return (int)Invoke((Func<int>)delegate
                {
                    if (busy || !host.Running) return 0;
                    busy = true;
                    try { using (var approval = new Approval(name, pairing)) { approval.ShowDialog(this); return approval.Permission; } }
                    finally { busy = false; }
                });
            }
            catch (InvalidOperationException) { return 0; }
        }
        void Changed(string message)
        {
            if (IsDisposed || Disposing) return;
            if (InvokeRequired) { try { BeginInvoke((Action)delegate { Changed(message); }); } catch (InvalidOperationException) { } return; }
            latest = message; eventLabel.Text = message;
            // A pairing changes access; defer rendering until a modal confirmation has closed.
            if (page == "Access" && !busy) Render();
        }
        Panel Header(string title, string subtitle)
        {
            var p = new Panel { Dock = DockStyle.Top, Height = 104 };
            var small = Theme.Label(subtitle, 10, Theme.Muted); small.Dock = DockStyle.Bottom; small.Height = 38;
            p.Controls.Add(small); p.Controls.Add(Theme.Label(title, 27, null, FontStyle.Bold)); return p;
        }
        void Render()
        {
            content.SuspendLayout(); foreach (Control c in content.Controls.Cast<Control>().ToArray()) c.Dispose(); content.Controls.Clear(); hostState = null;
            foreach (var n in navigation) { n.Value.BackColor = n.Key == page ? Color.FromArgb(30, 62, 61) : Theme.Sidebar; n.Value.ForeColor = n.Key == page ? Theme.Accent : Theme.Muted; n.Value.FlatAppearance.BorderSize = 0; }
            if (page == "Computers") Computers(); else if (page == "This computer") ThisComputer(); else if (page == "Access") Access(); else if (page == "Family account") content.Controls.Add(new AccountPanel(store, account, host)); else if (page == "Wake a computer") content.Controls.Add(new WakePanel()); else About();
            content.ResumeLayout(true);
        }
        internal void SelectPage(string name) { if (!navigation.ContainsKey(name)) throw new ArgumentException("Unknown page."); page = name; Render(); }
        void Computers()
        {
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Padding = new Padding(0, 4, 0, 0) };
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 58)); body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 42));
            var devices = new Panel { Dock = DockStyle.Fill, Padding = new Padding(0, 0, 20, 0) };
            var add = Theme.Button("+  Add a computer", true); add.Dock = DockStyle.Top; add.Height = 45; add.Click += delegate { using (var dialog = new PairDialog(store)) dialog.ShowDialog(this); Render(); };
            var list = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, Padding = new Padding(0, 15, 0, 0) };
            Device[] saved; lock (store.Sync) saved = store.Data.Devices.ToArray();
            if (saved.Length == 0)
            {
                var empty = new Card { Width = 455, Height = 300, Margin = new Padding(0, 0, 0, 15) };
                var t = Theme.Label("Ready when you are", 17, null, FontStyle.Bold); t.Height = 58;
                var s = Theme.Label("Pair with a computer you own or one shared with you. It stays in this list, ready for your next approved connection.", 11, Theme.Muted); s.Height = 125;
                var badge = Theme.Label("No public discovery. No default passwords.", 9, Theme.Accent); badge.Height = 48;
                empty.Controls.Add(badge); empty.Controls.Add(s); empty.Controls.Add(t); list.Controls.Add(empty);
            }
            foreach (var device in saved)
            {
                var item = new Card { Width = 455, Height = 202, Margin = new Padding(0, 0, 0, 15) };
                var title = Theme.Label(device.Name, 16, null, FontStyle.Bold); title.Height = 38;
                var address = Theme.Label(device.Address + ":" + device.Port + "\n" + (device.Control ? "Granted: view and control" : "Granted: view only"), 10, Theme.Muted); address.Height = 65;
                var actions = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 48, WrapContents = false };
                var connect = Theme.Button("Connect", true); connect.Width = 125; connect.Click += delegate { new Viewer(store, device).Show(this); };
                var edit = Theme.Button("Address"); edit.Width = 100; edit.Click += delegate { EditAddress(device); };
                var remove = Theme.Button("Forget"); remove.Width = 90; remove.Click += delegate { lock (store.Sync) { store.Data.Devices.Remove(device); store.Save(); } Render(); };
                actions.Controls.Add(connect); actions.Controls.Add(edit); actions.Controls.Add(remove);
                item.Controls.Add(actions); item.Controls.Add(address); item.Controls.Add(title); list.Controls.Add(item);
            }
            list.SizeChanged += delegate { foreach (Control c in list.Controls) c.Width = Math.Max(260, list.ClientSize.Width - 20); };
            devices.Controls.Add(list); devices.Controls.Add(add); body.Controls.Add(devices, 0, 0);
            body.Controls.Add(HostCard(false), 1, 0); content.Controls.Add(body); content.Controls.Add(Header("Your computers", "Private access to the screens that matter to you."));
        }
        Card HostCard(bool detailed)
        {
            var card = new Card { Dock = DockStyle.Top, Height = 470 };
            var title = Theme.Label("This computer", 17, null, FontStyle.Bold); title.Height = 45;
            var name = Theme.Label(store.Data.Name, 12); name.Height = 38;
            hostState = Theme.Label(host.Active ? "●  Someone is connected" : host.Running ? "●  Hosting is on" : "○  Hosting is off", 10, host.Running ? Theme.Accent : Theme.Muted); hostState.Height = 40;
            var description = Theme.Label("Share this screen with a trusted computer. You approve each session locally.", 10, Theme.Muted); description.Height = 70;
            var start = Theme.Button(host.Running ? "Turn hosting off" : "Start hosting", !host.Running); start.Dock = DockStyle.Top; start.Click += delegate { try { if (host.Running) host.Stop(); else host.Start(IPAddress.Any, store.Data.Port); Render(); } catch (Exception ex) { Theme.Error(this, ex); } };
            var spacer = new Panel { Dock = DockStyle.Top, Height = 14 };
            var invite = Theme.Button("Create invitation"); invite.Dock = DockStyle.Top; invite.Enabled = host.Running; invite.Click += delegate { Invite(); };
            var info = Theme.Label("Direct connection · LAN or private VPN\nNo automatic router or firewall changes\n\nEncrypted · Paired devices only", 9, Theme.Muted); info.Height = 130; info.Padding = new Padding(0, 14, 0, 0);
            card.Controls.Add(info); card.Controls.Add(invite); card.Controls.Add(spacer); card.Controls.Add(start); card.Controls.Add(description); card.Controls.Add(hostState); card.Controls.Add(name); card.Controls.Add(title);
            return card;
        }
        void Invite()
        {
            using (var dialog = new Form { Text = "Hyperlink · Share an invitation", ClientSize = new Size(600, 365), BackColor = Theme.Background, ForeColor = Theme.Text, StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false })
            {
                var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(25), ColumnCount = 1, RowCount = 6 };
                foreach (int height in new[] { 44, 45, 35, 92, 45, 44 }) layout.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
                layout.Controls.Add(Theme.Label("Invite a trusted computer", 17, null, FontStyle.Bold), 0, 0);
                layout.Controls.Add(Theme.Label("Reachable address on your LAN or private VPN", 10, Theme.Muted), 0, 1);
                var address = Theme.Field(Util.LanAddress()); layout.Controls.Add(address, 0, 2);
                var result = Theme.Field(""); result.ReadOnly = true; result.Multiline = true; result.Dock = DockStyle.Fill; result.ScrollBars = ScrollBars.Vertical; layout.Controls.Add(result, 0, 3);
                var note = Theme.Label("Single-use · Five-minute expiry · Local pairing approval", 9, Theme.Muted); note.Padding = new Padding(0, 12, 0, 0); layout.Controls.Add(note, 0, 4);
                var create = Theme.Button("Generate and copy invitation", true); create.Dock = DockStyle.Fill;
                create.Click += delegate { try { var invitation = host.Invite(address.Text); result.Text = invitation.Encode(); Clipboard.SetText(result.Text); note.Text = "Copied. Share privately, then approve pairing here."; note.ForeColor = Theme.Accent; } catch (Exception ex) { Theme.Error(dialog, ex); } };
                layout.Controls.Add(create, 0, 5); Theme.FitLabels(layout); dialog.Controls.Add(layout); dialog.ShowDialog(this);
            }
        }
        void EditAddress(Device device)
        {
            using (var dialog = new Form { Text = "Hyperlink · Reachable address", ClientSize = new Size(420, 220), BackColor = Theme.Background, StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false })
            {
                var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(24), RowCount = 3 };
                layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 70)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 35)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 45));
                layout.Controls.Add(Theme.Label("Update the LAN or VPN address. This computer's pinned identity stays the same.", 11, Theme.Muted), 0, 0);
                var input = Theme.Field(device.Address); layout.Controls.Add(input, 0, 1); var save = Theme.Button("Save address", true); save.Dock = DockStyle.Fill;
                save.Click += delegate { if (Uri.CheckHostName(input.Text.Trim()) == UriHostNameType.Unknown) { MessageBox.Show(dialog, "Enter a valid IP address or hostname."); return; } lock (store.Sync) { device.Address = input.Text.Trim(); store.Save(); } dialog.Close(); Render(); };
                layout.Controls.Add(save, 0, 2); Theme.FitLabels(layout); dialog.Controls.Add(layout); dialog.ShowDialog(this);
            }
        }
        void ThisComputer()
        {
            var row = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2 }; row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50)); row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            var hosting = HostCard(true); hosting.Margin = new Padding(0, 0, 20, 0); row.Controls.Add(hosting, 0, 0);
            var settings = new Card { Dock = DockStyle.Top, Height = 500 };
            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 8 };
            foreach (int height in new[] { 42, 25, 36, 28, 36, 42, 60, 78 }) layout.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
            layout.Controls.Add(Theme.Label("Host settings", 17, null, FontStyle.Bold), 0, 0); layout.Controls.Add(Theme.Label("Computer name", 9, Theme.Muted), 0, 1);
            var name = Theme.Field(store.Data.Name); layout.Controls.Add(name, 0, 2); layout.Controls.Add(Theme.Label("Port · Change while hosting is off", 9, Theme.Muted), 0, 3);
            var port = Theme.Field(store.Data.Port.ToString()); port.Enabled = !host.Running; layout.Controls.Add(port, 0, 4);
            var save = Theme.Button("Save settings", true); save.Dock = DockStyle.Fill; save.Click += delegate { try { int p; if (!Int32.TryParse(port.Text, out p) || p < 1024 || p > 65535) throw new ArgumentException("Choose a port from 1024 to 65535."); lock (store.Sync) { store.Data.Name = Util.Name(name.Text); if (!host.Running) store.Data.Port = p; store.Save(); } Render(); } catch (Exception ex) { Theme.Error(this, ex); } }; layout.Controls.Add(save, 0, 5);
            var monitors = new ComboBox { Dock = DockStyle.Top, DropDownStyle = ComboBoxStyle.DropDownList, BackColor = Theme.Card, ForeColor = Theme.Text, Enabled = !host.Active, Margin = new Padding(0, 18, 0, 0), DrawMode = DrawMode.OwnerDrawFixed, ItemHeight = 28, FlatStyle = FlatStyle.Flat };
            monitors.DrawItem += delegate(object sender, DrawItemEventArgs e) { if (e.Index < 0) return; using (var b = new SolidBrush(Theme.Card)) e.Graphics.FillRectangle(b, e.Bounds); TextRenderer.DrawText(e.Graphics, monitors.Items[e.Index].ToString(), monitors.Font, e.Bounds, Theme.Text, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis); };
            int index = 0; foreach (var screen in Screen.AllScreens) { monitors.Items.Add("Display " + (++index) + " · " + screen.Bounds.Width + " × " + screen.Bounds.Height + (screen.Primary ? " · Primary" : "")); } monitors.SelectedIndex = Math.Min(host.Monitor, monitors.Items.Count - 1); monitors.SelectedIndexChanged += delegate { if (!host.Active) host.Monitor = monitors.SelectedIndex; }; layout.Controls.Add(monitors, 0, 6);
            layout.Controls.Add(Theme.Label("Changing display applies to the next session. Lock screens and UAC secure desktops require local action in this draft.", 9, Theme.Muted), 0, 7);
            Theme.FitLabels(layout); settings.Controls.Add(layout); row.Controls.Add(settings, 1, 0); content.Controls.Add(row); content.Controls.Add(Header("This computer", "You decide when this computer is available and who gets access."));
        }
        void Access()
        {
            var list = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true };
            Peer[] peers; lock (store.Sync) peers = store.Data.Peers.ToArray();
            if (peers.Length == 0) { var c = new Card { Width = 750, Height = 180 }; var s = Theme.Label("No computers have access yet.\n\nCreate an invitation on This computer, then approve pairing locally. Each connection also asks for your approval.", 12, Theme.Muted); s.Height = 140; c.Controls.Add(s); list.Controls.Add(c); }
            foreach (var peer in peers)
            {
                var c = new Card { Width = 750, Height = 220, Margin = new Padding(0, 0, 0, 14) };
                var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
                layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 64));
                var labels = new Panel { Dock = DockStyle.Fill };
                var info = Theme.Label((peer.Control ? "View and control" : "View only") + " · Local approval required\nIdentity: " + peer.Id.Substring(0, 16), 9, Theme.Muted); info.Height = 58;
                labels.Controls.Add(info); labels.Controls.Add(Theme.Label(peer.Name, 16, null, FontStyle.Bold)); layout.Controls.Add(labels, 0, 0);
                var actions = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
                var permissions = Theme.Button("Permissions and shared folder"); permissions.Width = 280; permissions.Height = 34;
                permissions.Click += delegate { using (var dialog = new ExtensionPermissions(store, host, peer)) dialog.ShowDialog(this); Render(); }; actions.Controls.Add(permissions);
                var revoke = Theme.Button("Revoke access"); revoke.ForeColor = Theme.Red; revoke.Width = 155; revoke.Height = 34;
                revoke.Click += delegate { host.Revoke(peer.Id); Render(); }; actions.Controls.Add(revoke);
                layout.Controls.Add(actions, 0, 1); c.Controls.Add(layout); list.Controls.Add(c);
            }
            list.SizeChanged += delegate { foreach (Control c in list.Controls) c.Width = Math.Max(400, list.ClientSize.Width - 20); };
            content.Controls.Add(list); content.Controls.Add(Header("Access to this computer", "Every grant is specific to a paired computer. Revoking also stops its active session."));
        }
        void About()
        {
            var card = new Card { Dock = DockStyle.Fill }; var text = new TextBox { Text = "Hyperlink 0.5.6\r\n\r\nA working, attended Windows draft.\r\n\r\nAVAILABLE NOW\r\nLive screen viewing and pointer / keyboard control\r\nTLS 1.2 with pinned certificates and signed device challenges\r\nOne-time invitations, per-device grants, local approval and revocation\r\nWindows-protected identity storage and a local Stop button\r\nOwner-authorized files, text clipboard and system audio\r\nSigned update packages, restart checks and recovery backups\r\nLocal Wake-on-LAN and owner-authorized Windows video and optional system-audio recording\r\nOwner-authorized local display privacy\r\n\r\nDRAFT LIMITS\r\nJPEG capture, maximum 1600 × 1000, 30 fps requested cap\r\nDelivered frame rate is measured in the viewer; 120 fps is unverified\r\nDirect LAN / private VPN only; no rendezvous or relay\r\nInvite-only account controller tested; public domain deployment pending\r\nAndroid APK built; on-device launch unverified\r\nUnattended service and UAC / lock-screen control remain unfinished\r\n\r\nUse only for a private evaluation with trusted computers.\r\nThis unsigned draft is not the security-audited family release.", Font = Theme.Font(11), ForeColor = Theme.Muted, BackColor = Theme.Card, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, BorderStyle = BorderStyle.None, Dock = DockStyle.Fill }; var updateLayout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 }; updateLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); updateLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 56)); var updateButton = Theme.Button("Install verified local update", true); updateButton.Dock = DockStyle.Fill; updateButton.Click += delegate { using (var window = new UpdateWindow(this, store)) window.ShowDialog(this); }; updateLayout.Controls.Add(text, 0, 0); updateLayout.Controls.Add(updateButton, 0, 1); card.Controls.Add(updateLayout); content.Controls.Add(card); content.Controls.Add(Header("Built for your own computers", "First draft · Native Windows · No installer or cloud signup."));
        }
    }

    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr context);
        [STAThread] static int Main(string[] args)
        {
            try
            {
                try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch (EntryPointNotFoundException) { }
                if (args.Length > 0 && args[0] == "--self-test") return SelfTest.Run(args.Length > 1 ? args[1] : null);
            if (args.Length == 2 && args[0] == "--video-self-test") return VideoTests.Run(args[1]);
            if (args.Length == 2 && args[0] == "--recording-self-test") { Console.WriteLine(RecordingTests.Run(args[1])); return 0; }
            if (args.Length == 2 && args[0] == "--privacy-self-test") return PrivacyTests.ScreenCheck(args[1]);
                if (args.Length == 3 && args[0] == "--verify-update") { ReleasePackages.Stage(args[1], args[2], UpdateCoordinator.Floor); return 0; }
                if (args.Length == 2 && args[0] == "--install-local-update") { string update = UpdateCoordinator.Prepare(args[1]); UpdateCoordinator.Start(update); return 0; }
                if (args.Length == 0 || (args.Length > 0 && args[0].StartsWith("--update-", StringComparison.Ordinal)))
                { int? updateResult = UpdateCoordinator.Startup(args); if (updateResult.HasValue) return updateResult.Value; }
                if (args.Length == 3 && args[0] == "--screen-test") return SelfTest.ScreenCheck(args[1], args[2]);
                Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
                if (args.Length == 3 && args[0] == "--viewer-smoke") return SelfTest.ViewerCheck(args[1], args[2]);
                string data = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Data");
                if (args.Length == 2 && args[0] == "--data") data = args[1];
                string mutexName = "Local\\Hyperlink-" + Util.Hash(Path.GetFullPath(data).ToUpperInvariant() + System.Security.Principal.WindowsIdentity.GetCurrent().User.Value);
                using (var instance = new Mutex(false, mutexName))
                {
                    bool acquired;
                    try { acquired = instance.WaitOne(0); } catch (AbandonedMutexException) { acquired = true; }
                    if (!acquired) { MessageBox.Show("Hyperlink is already open for this data folder.", "Hyperlink", MessageBoxButtons.OK, MessageBoxIcon.Information); return 0; }
                    try
                    {
                        using (var store = new Store(data)) using (var main = new MainWindow(store))
                        {
                            main.Shown += delegate { UpdateCoordinator.MarkHealthy(); };
                            UpdateFeed.Attach(main, store);
                            if (args.Length >= 2 && args[0] == "--smoke-ui")
                            {
                                if (args.Length == 3) main.SelectPage(args[2]);
                                var timer = new System.Windows.Forms.Timer { Interval = 1200 };
                                timer.Tick += delegate { timer.Stop(); using (var b = new Bitmap(main.Width, main.Height)) { main.DrawToBitmap(b, new Rectangle(Point.Empty, main.Size)); b.Save(args[1], System.Drawing.Imaging.ImageFormat.Png); } main.Close(); timer.Dispose(); };
                                main.Shown += delegate { timer.Start(); };
                            }
                        Application.Run(main);
                        JpegRecording.FinishAll();
                        }
                    }
                    finally { instance.ReleaseMutex(); }
                }
                return 0;
            }
            catch (Exception ex)
            {
                if (args.Length > 0 && new[] { "--smoke-ui", "--verify-update", "--install-local-update" }.Contains(args[0])) Console.Error.WriteLine(ex.ToString());
                else MessageBox.Show(ex.Message, "Hyperlink couldn't start", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
        }
    }
}
