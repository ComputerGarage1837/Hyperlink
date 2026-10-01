using System;
using System.Drawing;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class PrivacyWindow : Form
    {
        readonly Remote remote; readonly ExtensionClient client;
        readonly Button start, stop; readonly Label status;
        bool active, busy, closed;
        PrivacyWindow(Remote connection)
        {
            remote = connection; client = new ExtensionClient(remote);
            Text = "Local display privacy"; Size = new Size(590, 310); StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Background; ForeColor = Theme.Text; Font = Theme.Font(10);
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), ColumnCount = 1 };
            var help = Theme.Label("With the owner's separate permission, the host covers its local displays while your session remains visible here. The local owner can press Ctrl+Alt+Shift+H to restore the displays and disconnect. Privacy ends automatically after 30 minutes, on disconnect, or when this window closes.", 11, Theme.Muted);
            help.AutoSize = false; help.Height = 115; help.Dock = DockStyle.Top; body.Controls.Add(help);
            var buttons = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 50 };
            start = Theme.Button("Start privacy", true); start.Width = 230; start.Click += async delegate { await Change(true); }; buttons.Controls.Add(start);
            stop = Theme.Button("Restore displays"); stop.Width = 230; stop.Enabled = false; stop.Click += async delegate { await Change(false); }; buttons.Controls.Add(stop); body.Controls.Add(buttons);
            status = Theme.Label("Privacy is off.", 10, Theme.Muted); status.AutoSize = false; status.Height = 50; status.Dock = DockStyle.Top; body.Controls.Add(status); Controls.Add(body);
            remote.Ended += Ended;
            FormClosed += delegate { closed = true; remote.Ended -= Ended; if (!busy) Cleanup(); };
        }
        public static void ShowFor(Remote remote, Form owner)
        {
            foreach (Form form in Application.OpenForms) { var window = form as PrivacyWindow; if (window != null && window.remote == remote) { window.Activate(); return; } }
            new PrivacyWindow(remote).Show(owner);
        }
        async Task Change(bool enable)
        {
            if (busy || active == enable) return;
            busy = true; start.Enabled = stop.Enabled = false;
            try { await Task.Run(() => client.Call(enable ? "privacy-start" : "privacy-stop")); active = enable; if (!closed) status.Text = enable ? "Privacy active. The host's emergency hotkey remains available." : "Privacy is off."; }
            catch (Exception ex) { if (!enable) remote.Dispose(); if (!closed) status.Text = ex.Message; }
            busy = false;
            if (closed) Cleanup(); else { start.Enabled = !active && remote.Connected; stop.Enabled = active && remote.Connected; }
        }
        void Cleanup()
        {
            Task.Run(delegate { try { if (active && remote.Connected) client.Call("privacy-stop"); } catch { remote.Dispose(); } finally { client.Dispose(); } });
        }
        void Ended(string reason)
        {
            if (!closed && IsHandleCreated) try { BeginInvoke((Action)delegate { active = false; start.Enabled = stop.Enabled = false; status.Text = "Disconnected; the host restores its displays."; }); } catch (InvalidOperationException) { }
        }
    }
}
