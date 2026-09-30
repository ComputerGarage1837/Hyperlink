using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class AccountPanel : Panel
    {
        readonly Store store;
        readonly AccountClient client;
        readonly Host host;
        readonly FlowLayoutPanel body;
        readonly Label status;
        readonly TextBox username, password, code;
        readonly CheckBox remember;
        readonly CheckBox trust;
        bool busy;
        public AccountPanel(Store state, AccountClient account, Host hosting)
        {
            store = state; client = account; host = hosting; Dock = DockStyle.Fill; Padding = new Padding(24); BackColor = Color.FromArgb(18, 24, 36);
            body = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true };
            Controls.Add(body);
            AddLabel("Family account", 24, FontStyle.Bold);
            AddLabel("hyperlink.myfamilyapps.ca · Invite-only access", 11, FontStyle.Regular);
            AddLabel("The domain is being prepared. Account requests will work after deployment.", 10, FontStyle.Regular);
            username = Field("Username", false); password = Field("Password", true); code = Field("Authenticator code", false);
            remember = new CheckBox { Text = "Remember this login on this Windows user account", AutoSize = true, ForeColor = Color.White, Margin = new Padding(0, 8, 0, 8) };
            body.Controls.Add(remember);
            trust = new CheckBox { Text = "Approve this computer as a new trusted viewer", AutoSize = true, ForeColor = Color.White, Margin = new Padding(0, 0, 0, 8) };
            body.Controls.Add(trust);
            var row = new FlowLayoutPanel { AutoSize = true, WrapContents = true, Width = 800, Margin = new Padding(0, 8, 0, 8) };
            body.Controls.Add(row);
            Button(row, "Sign in", delegate {
                string name = username.Text, pass = password.Text, otp = code.Text; bool save = remember.Checked, approve = trust.Checked;
                password.Clear(); code.Clear();
                Run(delegate { string refresh = client.Login(name, pass, otp, approve); lock (store.Sync) { store.Data.AccountRefresh = save ? refresh : null; store.Save(); } }, RefreshDevices);
            });
            Button(row, "Restore saved login", delegate {
                string token; lock (store.Sync) token = store.Data.AccountRefresh;
                if (String.IsNullOrEmpty(token)) { status.Text = "No remembered login. Sign in first."; return; }
                Run(delegate { string next = client.Refresh(token); lock (store.Sync) { store.Data.AccountRefresh = next; store.Save(); } }, RefreshDevices);
            });
            Button(row, "Sign out", delegate {
                lock (store.Sync) { store.Data.AccountRefresh = null; store.Save(); }
                Run(delegate { client.Logout(); }, delegate { status.Text = "Signed out. Saved login removed."; });
            });
            Button(row, "Accept invitation", AcceptInvitation);
            Button(row, "Recovery", Recover);
            var actions = new FlowLayoutPanel { AutoSize = true, WrapContents = true, Width = 800, Margin = new Padding(0, 8, 0, 8) }; body.Controls.Add(actions);
            Button(actions, "Refresh computers", RefreshDevices);
            Button(actions, "Enroll this computer", delegate {
                if (MessageBox.Show(this, "Enroll this host with your family account? The temporary direct-pairing transport will be disabled. Hosted connections are still being integrated.", "Enroll host", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
                Run(delegate {
                    host.Stop();
                    var result = client.Call("POST", "/v1/devices", new { name = store.Data.Name, public_jwk = client.PublicJwk(), fingerprint = store.Fingerprint }, true);
                    lock (store.Sync) { store.Data.FamilyDeviceId = (string)result["device_id"]; store.Save(); }
                }, RefreshDevices);
            });
            Button(actions, "Invite family member", delegate {
                string name = Prompt("Invite family member", "Intended username (invitation expires in 24 hours)", false); if (name == null) return;
                Dictionary<string, object> result = null;
                Run(delegate { result = client.Call("POST", "/v1/invites", new { username = name }, true); }, delegate { ShowSecret("Single-use invitation for " + name, (string)result["invite"]); });
            });
            Button(actions, "Verify MFA again", delegate {
                string otp = Prompt("Verify account", "Current authenticator code", false); if (otp == null) return;
                Run(delegate { client.Call("POST", "/v1/elevate", new { code = otp }, true); }, delegate { status.Text = "MFA verified. Sensitive changes allowed for five minutes."; });
            });
            Button(actions, "Manage logins", ManageLogins);
            Button(actions, "Trusted viewers", ManageViewers);
            status = AddLabel(client.SignedIn ? "Signed in. Refresh to load your computers." : "Sign in or accept a personal invitation.", 11, FontStyle.Regular);
            AddLabel("Account enrollment does not enable unattended access. The host still requires local consent.", 10, FontStyle.Regular);
        }
        Label AddLabel(string text, float size, FontStyle style)
        {
            var label = new Label { Text = text, AutoSize = true, MaximumSize = new Size(800, 0), ForeColor = Color.FromArgb(222, 229, 242), Font = new Font("Segoe UI", size, style), Margin = new Padding(0, 5, 0, 10) };
            body.Controls.Add(label); return label;
        }
        TextBox Field(string label, bool secret)
        {
            AddLabel(label, 10, FontStyle.Regular);
            var field = new TextBox { Width = 370, Font = new Font("Segoe UI", 12), UseSystemPasswordChar = secret, BackColor = Color.FromArgb(30, 39, 54), ForeColor = Color.White, Margin = new Padding(0, 0, 0, 8) };
            body.Controls.Add(field); return field;
        }
        void Button(Control parent, string text, Action action)
        {
            var button = new Button { Text = text, AutoSize = true, Height = 36, FlatStyle = FlatStyle.Flat, ForeColor = Color.White, BackColor = Color.FromArgb(42, 58, 83), Padding = new Padding(8, 3, 8, 3) };
            button.Click += delegate { if (!busy) action(); }; parent.Controls.Add(button);
        }
        void Run(Action work, Action complete)
        {
            if (busy) return; busy = true; status.Text = "Contacting the family server…";
            ThreadPool.QueueUserWorkItem(delegate {
                string error = null; try { work(); } catch (Exception e) { error = e.Message; }
                if (IsDisposed || !IsHandleCreated) return;
                try { BeginInvoke((Action)delegate { if (IsDisposed) return; busy = false; if (error != null) status.Text = error; else complete(); }); }
                catch (InvalidOperationException) { }
            });
        }
        void AcceptInvitation()
        {
            string invite = Prompt("Accept invitation", "Your personal invitation", true); if (invite == null) return;
            string name = username.Text, pass = password.Text; password.Clear(); Dictionary<string, object> enrollment = null;
            Run(delegate { enrollment = client.Call("POST", "/v1/register", new { invite, username = name, password = pass }, false); }, delegate { Activate(enrollment); });
        }
        void Activate(Dictionary<string, object> enrollment)
        {
            ShowSecret("Add Hyperlink to your authenticator", "Account: " + username.Text + "\r\nSecret: " + enrollment["secret"] + "\r\n\r\n" + enrollment["uri"] + "\r\n\r\nEnrollment expires in ten minutes.");
            string otp = Prompt("Complete enrollment", "Code from your authenticator", false); if (otp == null) return;
            Dictionary<string, object> result = null;
            Run(delegate { result = client.Call("POST", "/v1/activate", new { enrollment_id = enrollment["enrollment_id"], code = otp }, false); }, delegate {
                var lines = new List<string>(); foreach (object value in (IEnumerable)result["recovery_codes"]) lines.Add((string)value);
                ShowSecret("Save your recovery codes privately", String.Join("\r\n", lines.ToArray())); status.Text = "Account ready. Sign in with the next authenticator code.";
            });
        }
        void Recover()
        {
            string recovery = Prompt("Recover account", "One unused recovery code. This revokes old logins and sharing trust.", true); if (recovery == null) return;
            string name = username.Text, pass = password.Text; password.Clear(); Dictionary<string, object> enrollment = null;
            Run(delegate { enrollment = client.Call("POST", "/v1/recover", new { username = name, password = pass, recovery_code = recovery }, false); }, delegate { client.Forget(); lock (store.Sync) { store.Data.AccountRefresh = null; store.Save(); } Activate(enrollment); });
        }
        void RefreshDevices()
        {
            Dictionary<string, object> result = null;
            Run(delegate { result = client.Call("GET", "/v1/devices", null, true); }, delegate {
                foreach (Control old in new List<Control>(GetRows())) { body.Controls.Remove(old); old.Dispose(); }
                int count = 0;
                foreach (Dictionary<string, object> device in (IEnumerable)result["devices"]) {
                    count++; var row = new FlowLayoutPanel { Width = 800, AutoSize = true, Tag = "device", Margin = new Padding(0, 8, 0, 8) };
                    row.Controls.Add(new Label { Text = (string)device["name"], AutoSize = true, ForeColor = Color.White, Padding = new Padding(0, 10, 10, 0) });
                    string id = (string)device["id"];
                    if (device["grant_id"] == null) {
                        if (device.ContainsKey("needs_reauthorization") && (bool)device["needs_reauthorization"]) Button(row, "Renew host trust", delegate {
                            Run(delegate { client.Call("POST", "/v1/devices/reauthorize", new { device_id = id }, true); }, RefreshDevices);
                        });
                        Button(row, "Share…", delegate { Share(id); });
                        Button(row, "Manage shares", delegate { ManageShares(id); });
                        Button(row, "Revoke computer", delegate {
                            if (MessageBox.Show(this, "Revoke this host identity and its active sessions?", "Revoke computer", MessageBoxButtons.YesNo) == DialogResult.Yes)
                                Run(delegate { client.Call("POST", "/v1/devices/revoke", new { device_id = id }, true); }, RefreshDevices);
                        });
                    } else row.Controls.Add(new Label { Text = "Shared with you · Host consent required", AutoSize = true, ForeColor = Color.LightSteelBlue, Padding = new Padding(0, 10, 0, 0) });
                    body.Controls.Add(row);
                }
                status.Text = count + " computer(s). Hosted remote connection integration is still in development.";
            });
        }
        IEnumerable<Control> GetRows() { foreach (Control c in body.Controls) if ((string)c.Tag == "device") yield return c; }
        void Share(string id)
        {
            string name = Prompt("Share computer", "Family member username", false); if (name == null) return;
            bool control = MessageBox.Show(this, "Allow keyboard and pointer control as well as viewing?", "Permissions", MessageBoxButtons.YesNo) == DialogResult.Yes;
            Dictionary<string, object> result = null;
            Run(delegate { result = client.Call("POST", "/v1/grants", new { device_id = id, username = name, rights = control ? new[] { "view", "control" } : new[] { "view" }, expires = (long)(DateTime.UtcNow.AddDays(7) - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds }, true); }, delegate {
                ShowSecret("Share created for seven days", "Grant ID: " + result["grant_id"] + "\r\n\r\nHost approval is still required for each connection.");
            });
        }
        void ManageLogins()
        {
            Dictionary<string, object> result = null;
            Run(delegate { result = client.Call("GET", "/v1/logins", null, true); }, delegate {
                var list = new List<string>(); foreach (Dictionary<string, object> s in (IEnumerable)result["logins"]) list.Add((string)s["id"] + " · revoked=" + s["revoked"]);
                ShowSecret("Your account logins", String.Join("\r\n", list.ToArray()));
                string id = Prompt("Revoke login", "Paste a login ID to revoke, or cancel", false); if (id == null) return;
                Run(delegate { client.Call("POST", "/v1/logins/revoke", new { session_id = id }, true); }, delegate { if (id == client.SessionId) client.Forget(); status.Text = "Login revoked."; });
            });
        }
        void ManageShares(string deviceId)
        {
            Dictionary<string, object> result = null;
            Run(delegate { result = client.Call("POST", "/v1/grants/list", new { device_id = deviceId }, true); }, delegate {
                var list = new List<string>();
                foreach (Dictionary<string, object> g in (IEnumerable)result["grants"]) {
                    var rights = new List<string>(); foreach (object r in (IEnumerable)g["rights"]) rights.Add((string)r);
                    list.Add(g["username"] + " · " + String.Join(", ", rights.ToArray()) + "\r\n" + g["id"] + " · revoked=" + g["revoked"]);
                }
                ShowSecret("Shares for this computer", String.Join("\r\n\r\n", list.ToArray()));
                string id = Prompt("Revoke share", "Paste a grant ID to revoke, or cancel. Sharing again replaces the previous rights and expiry.", false); if (id == null) return;
                Run(delegate { client.Call("POST", "/v1/grants/revoke", new { grant_id = id }, true); }, delegate { status.Text = "Share revoked. Active authorization will be denied on renewal."; });
            });
        }
        void ManageViewers()
        {
            Dictionary<string, object> result = null;
            Run(delegate { result = client.Call("GET", "/v1/viewers", null, true); }, delegate {
                var list = new List<string>(); foreach (Dictionary<string, object> v in (IEnumerable)result["viewers"]) list.Add(v["label"] + " · " + v["id"] + " · revoked=" + v["revoked"]);
                ShowSecret("Your trusted viewers", String.Join("\r\n", list.ToArray()));
                string id = Prompt("Revoke viewer", "Paste a viewer ID. Every login using that key will be revoked.", false); if (id == null) return;
                Run(delegate { client.Call("POST", "/v1/viewers/revoke", new { viewer_id = id }, true); }, delegate { status.Text = "Viewer key revoked. A replacement key requires explicit approval."; });
            });
        }
        static string Prompt(string title, string text, bool secret)
        {
            using (var form = new Form { Text = title, ClientSize = new Size(540, 160), StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog, MinimizeBox = false, MaximizeBox = false }) {
                var label = new Label { Text = text, Left = 16, Top = 16, Width = 510, Height = 40 };
                var value = new TextBox { Left = 16, Top = 60, Width = 505, UseSystemPasswordChar = secret };
                var ok = new Button { Text = "Continue", Left = 335, Top = 110, Width = 90, DialogResult = DialogResult.OK };
                var cancel = new Button { Text = "Cancel", Left = 435, Top = 110, Width = 85, DialogResult = DialogResult.Cancel };
                form.Controls.AddRange(new Control[] { label, value, ok, cancel }); form.AcceptButton = ok; form.CancelButton = cancel;
                return form.ShowDialog() == DialogResult.OK ? value.Text : null;
            }
        }
        static void ShowSecret(string title, string text)
        {
            using (var form = new Form { Text = title, ClientSize = new Size(680, 330), StartPosition = FormStartPosition.CenterParent }) {
                var value = new TextBox { Text = text, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font("Consolas", 11) };
                form.Controls.Add(value); form.ShowDialog();
            }
        }
    }
}
