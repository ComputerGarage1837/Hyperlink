using System;
using System.Drawing;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class PinPairDialog : Form
    {
        readonly Store store; readonly TextBox computer, pin; readonly Button authorize; bool busy;
        internal PinPairDialog(Store state)
        {
            store = state; Text = "Connect to a computer · Hyperlink"; Size = new Size(530, 340); StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Background; ForeColor = Theme.Text; Font = Theme.Font(11); FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false;
            var body = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(24) };
            body.Controls.Add(new Label { Text = "Enter the computer ID and PIN from its Windows\nUnattended access screen.", Width = 470, Height = 55 });
            body.Controls.Add(Theme.Label("Computer ID (8 digits)", 10, Theme.Muted)); computer = new TextBox { Width = 440, MaxLength = 8 }; body.Controls.Add(computer);
            body.Controls.Add(Theme.Label("Windows PIN", 10, Theme.Muted)); pin = new TextBox { Width = 440, MaxLength = 12, UseSystemPasswordChar = true }; body.Controls.Add(pin);
            authorize = Theme.Button("Authorize this computer", true); authorize.Width = 440; authorize.Margin = new Padding(0, 18, 0, 0); authorize.Click += async delegate { await Pair(); }; body.Controls.Add(authorize); Controls.Add(body);
            FormClosing += delegate(object sender, FormClosingEventArgs e) { if (busy) e.Cancel = true; };
        }
        async Task Pair()
        {
            if (busy) return; string id = computer.Text.Trim(), secret = pin.Text; pin.Clear(); busy = true; authorize.Enabled = false; authorize.Text = "Connecting…";
            try { await Task.Run(delegate { using (var remote = new Remote(store)) remote.PairPin(id, secret); }); busy = false; DialogResult = DialogResult.OK; Close(); }
            catch (Exception ex) { Theme.Error(this, ex); }
            finally { busy = false; if (!IsDisposed) { authorize.Enabled = true; authorize.Text = "Authorize this computer"; } }
        }
    }
}
