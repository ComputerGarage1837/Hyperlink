using System;
using System.Drawing;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace Hyperlink
{
    sealed class UnattendedPanel : UserControl
    {
        readonly Store store; readonly Host host; readonly Action startRelay, stopRelay; readonly Func<bool> online;
        readonly TextBox pin, confirm; readonly CheckBox startup; readonly Label computer, status;
        readonly Button enable, disable; readonly Timer refresh; bool busy;
        internal bool SavingPin { get { return busy; } }
        internal UnattendedPanel(Store state, Host desktop, Action start, Action stop, Func<bool> connected)
        {
            store = state; host = desktop; startRelay = start; stopRelay = stop; online = connected;
            Dock = DockStyle.Fill; BackColor = Theme.Background; ForeColor = Theme.Text; Font = Theme.Font(11);
            var body = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true, Padding = new Padding(28) };
            body.Controls.Add(Theme.Label("Unattended access", 25, Theme.Text));
            body.Controls.Add(new Label { Text = "Set a PIN here, then enter your computer ID and PIN on Android.\nYou can connect without approving each session.", Width = 680, Height = 70 });
            computer = Theme.Label("", 24, Theme.Accent); computer.AutoSize = false; computer.Size = new Size(680, 55); body.Controls.Add(computer);
            body.Controls.Add(Theme.Label("PIN (6–12 digits)", 11, Theme.Muted));
            pin = new TextBox { Width = 330, UseSystemPasswordChar = true, MaxLength = 12 }; body.Controls.Add(pin);
            body.Controls.Add(Theme.Label("Confirm PIN", 11, Theme.Muted));
            confirm = new TextBox { Width = 330, UseSystemPasswordChar = true, MaxLength = 12 }; body.Controls.Add(confirm);
            startup = new CheckBox { Text = "Start Hyperlink when I sign in to Windows", AutoSize = true, Checked = !store.Data.Unattended || store.Data.StartWithWindows, Margin = new Padding(0, 18, 0, 18) }; body.Controls.Add(startup);
            var buttons = new FlowLayoutPanel { Width = 680, Height = 60 };
            enable = Theme.Button(store.Data.Unattended ? "Save PIN and settings" : "Enable unattended access", true); enable.Width = 280; enable.Click += async delegate { await Save(); }; buttons.Controls.Add(enable);
            disable = Theme.Button("Turn unattended access off"); disable.Width = 280; disable.Click += delegate { Disable(); }; buttons.Controls.Add(disable); body.Controls.Add(buttons);
            status = Theme.Label("", 11, Theme.Muted); status.AutoSize = false; status.Size = new Size(680, 95); body.Controls.Add(status);
            body.Controls.Add(new Label { Text = "Keep Windows signed in. Login-screen and UAC control are not available yet.\n\nChanging the PIN removes phones previously authorized with it. Extra permissions\nfor files, clipboard, audio, recording and privacy are managed under Access.", Width = 680, Height = 125 });
            Controls.Add(body); refresh = new Timer { Interval = 1000 }; refresh.Tick += delegate { UpdateState(); }; refresh.Start(); UpdateState();
        }
        void UpdateState()
        {
            computer.Text = String.IsNullOrEmpty(store.Data.RelayComputerCode) ? "Computer ID: not assigned yet" : "Computer ID: " + store.Data.RelayComputerCode;
            disable.Enabled = !busy && store.Data.Unattended;
            if (!busy) status.Text = store.Data.Unattended ? (online() ? "Ready for connections. Closing the window keeps Hyperlink in the system tray." : "Connecting to your server…") : "Unattended access is off. Choose a PIN to enable it.";
        }
        internal static void Startup(bool enabled)
        {
            using (var key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run"))
            {
                if (enabled) key.SetValue("Hyperlink", "\"" + Application.ExecutablePath + "\" --background", RegistryValueKind.String);
                else
                {
                    string value = key.GetValue("Hyperlink") as string;
                    if (value != null && value.StartsWith("\"" + Application.ExecutablePath + "\"", StringComparison.OrdinalIgnoreCase)) key.DeleteValue("Hyperlink", false);
                }
            }
        }
        async Task Save()
        {
            if (busy) return; string value = pin.Text, second = confirm.Text; bool boot = startup.Checked;
            if ((!store.Data.Unattended || value.Length > 0) && (!PinGate.Valid(value) || value != second)) { Theme.Error(FindForm(), new ArgumentException("Enter matching PINs with 6–12 digits.")); return; }
            if (value.Length == 0 && second.Length != 0) { Theme.Error(FindForm(), new ArgumentException("Enter matching PINs.")); return; }
            busy = true; enable.Enabled = disable.Enabled = false; status.Text = "Saving your protected PIN…";
            try
            {
                stopRelay(); host.Stop();
                await Task.Run(delegate { host.ConfigurePin(value, boot); });
                Startup(boot); startRelay(); pin.Clear(); confirm.Clear(); enable.Text = "Save PIN and settings";
            }
            catch (Exception ex) { Theme.Error(FindForm(), ex); }
            finally { busy = false; if (!IsDisposed) { enable.Enabled = true; UpdateState(); } }
        }
        void Disable()
        {
            if (busy) return;
            try { stopRelay(); host.Stop(); host.DisablePin(); Startup(false); enable.Text = "Enable unattended access"; UpdateState(); }
            catch (Exception ex) { Theme.Error(FindForm(), ex); }
        }
        protected override void Dispose(bool disposing) { if (disposing) refresh.Dispose(); base.Dispose(disposing); }
    }
    public sealed partial class Host
    {
        internal void ConfigurePin(string pin, bool startup)
        {
            lock (store.Sync)
            {
                if (!String.IsNullOrEmpty(store.Data.FamilyDeviceId)) throw new InvalidOperationException("Remove family enrollment before switching to PIN access.");
                string previousSalt = store.Data.PinSalt, previousHash = store.Data.PinHash; bool previous = store.Data.Unattended, previousStartup = store.Data.StartWithWindows;
                var peers = store.Data.Peers.ToArray();
                try
                {
                    if (!String.IsNullOrEmpty(pin)) { PinGate.Configure(store.Data, pin); store.Data.Peers.RemoveAll(p => p.Unattended); }
                    else if (!previous || String.IsNullOrEmpty(previousHash)) throw new ArgumentException("Choose a PIN first.");
                    store.Data.Unattended = true; store.Data.StartWithWindows = startup; store.Save(); pinGate.Reset();
                }
                catch { store.Data.PinSalt = previousSalt; store.Data.PinHash = previousHash; store.Data.Unattended = previous; store.Data.StartWithWindows = previousStartup; store.Data.Peers.Clear(); store.Data.Peers.AddRange(peers); throw; }
            }
        }
        internal void DisablePin()
        {
            lock (store.Sync) { bool previous = store.Data.Unattended; try { store.Data.Unattended = false; store.Save(); } catch { store.Data.Unattended = previous; throw; } }
        }
    }
    sealed partial class MainWindow
    {
        RelayHost relayHost; NotifyIcon tray; bool exiting;
        void StartUnattended()
        {
            if (!store.Data.Unattended) return;
            if (!host.Running) host.Start(System.Net.IPAddress.Any, store.Data.Port);
            StopRelay(); relayHost = new RelayHost(store, host, Changed);
            if (tray == null)
            {
                tray = new NotifyIcon { Icon = SystemIcons.Application, Text = "Hyperlink · Unattended access", Visible = true };
                tray.DoubleClick += delegate { Show(); WindowState = FormWindowState.Normal; Activate(); };
                var menu = new ContextMenuStrip(); menu.Items.Add("Open Hyperlink", null, delegate { Show(); WindowState = FormWindowState.Normal; Activate(); });
                menu.Items.Add("Stop hosting and exit", null, delegate { exiting = true; Close(); }); tray.ContextMenuStrip = menu;
            }
        }
        void StopRelay() { if (relayHost != null) { relayHost.Dispose(); relayHost = null; } }
        void UnattendedClosing(FormClosingEventArgs e)
        {
            if (!exiting && e.CloseReason == CloseReason.UserClosing && store.Data.Unattended && host.Running) { e.Cancel = true; Hide(); return; }
            StopRelay(); if (tray != null) { tray.Visible = false; tray.Dispose(); tray = null; }
        }
    }
}
