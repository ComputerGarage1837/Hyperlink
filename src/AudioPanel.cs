using System;
using System.Drawing;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    sealed class AudioPanel : Form
    {
        readonly Remote remote;
        readonly ExtensionClient client;
        readonly Label status;
        readonly Button start, stop;
        readonly object sync = new object();
        SystemAudioPlayback player;
        volatile bool closed, starting;
        public AudioPanel(Remote connection)
        {
            remote = connection; client = new ExtensionClient(remote); remote.AudioFrame += Receive;
            Text = "System audio · Hyperlink"; Size = new Size(560, 250); StartPosition = FormStartPosition.CenterParent;
            BackColor = Theme.Background; ForeColor = Theme.Text;
            Font = Theme.Font(10);
            var body = new FlowLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), FlowDirection = FlowDirection.TopDown, WrapContents = false }; Controls.Add(body);
            body.Controls.Add(new Label { Text = "Listen to host playback only. The owner must grant audio access.\nMicrophone and camera capture are not used.", Width = 510, Height = 50 });
            status = new Label { Text = "Audio is off.", Width = 510, Height = 30 }; body.Controls.Add(status);
            var buttons = new FlowLayoutPanel { Width = 510, Height = 50 }; body.Controls.Add(buttons);
            start = Theme.Button("Start listening"); start.Width = 220; buttons.Controls.Add(start);
            stop = Theme.Button("Stop listening"); stop.Width = 220; stop.Enabled = false; buttons.Controls.Add(stop);
            start.Click += delegate { StartListening(); }; stop.Click += delegate { StopListening(); };
            FormClosed += delegate
            {
                closed = true; remote.AudioFrame -= Receive; DisposePlayer();
                Task.Run(delegate { StopHost(); if (!starting) client.Dispose(); });
            };
        }
        void Post(Action action) { if (!closed && IsHandleCreated) try { BeginInvoke(action); } catch (InvalidOperationException) { } }
        public static void ShowFor(Remote remote, Form owner)
        {
            foreach (Form form in Application.OpenForms) { var audio = form as AudioPanel; if (audio != null && audio.remote == remote) { audio.Activate(); return; } }
            new AudioPanel(remote).Show(owner);
        }
        void Receive(byte[] packet) { SystemAudioPlayback value; lock (sync) value = player; if (value != null) value.Add(packet); }
        void DisposePlayer() { SystemAudioPlayback value; lock (sync) { value = player; player = null; } if (value != null) Task.Run(delegate { value.Dispose(); }); }
        void StopHost() { try { client.Call("audio-stop"); } catch { remote.Dispose(); } }
        void StartListening()
        {
            if (closed || starting) return; starting = true; start.Enabled = false; status.Text = "Requesting authorized playback…";
            Task.Run(delegate
            {
                try
                {
                    if (closed) return; var descriptor = client.Call("audio-start");
                    if (closed) return; var value = new SystemAudioPlayback(descriptor);
                    lock (sync) { if (!closed) { player = value; value = null; } }
                    if (value != null) value.Dispose();
                    Post(delegate { status.Text = "Listening to host system playback."; stop.Enabled = true; });
                }
                catch { DisposePlayer(); StopHost(); Post(delegate { status.Text = "Audio unavailable or not allowed by the owner."; start.Enabled = true; }); }
                finally { starting = false; if (closed) { StopHost(); client.Dispose(); } }
            });
        }
        void StopListening()
        {
            stop.Enabled = false; DisposePlayer(); status.Text = "Stopping audio…";
            Task.Run(delegate { StopHost(); Post(delegate { status.Text = "Audio is off."; start.Enabled = true; }); });
        }
    }
}
