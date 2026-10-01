using System;
using System.IO;
using System.Net;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Hyperlink
{
    static class UpdateFeed
    {
        const string Origin = "https://hyperlink.myfamilyapps.ca";
        static byte[] Metadata(CancellationToken cancel)
        {
            using (var output = new MemoryStream()) { Download(Origin + "/releases/windows/stable.json", output, 32768, cancel); return output.ToArray(); }
        }
        internal static ReleaseManifest Latest(CancellationToken cancel)
        {
            var manifest = ReleasePackages.Verify(Metadata(cancel), -1, ReleaseKey.PublicXml);
            if (manifest.sequence <= UpdateCoordinator.Floor || Version.Parse(manifest.version) <= System.Reflection.Assembly.GetExecutingAssembly().GetName().Version) return null;
            return manifest;
        }
        internal static string PrepareLatest(CancellationToken cancel)
        {
            var manifest = Latest(cancel); if (manifest == null) return null;
            string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Hyperlink", "Updates", "downloads");
            Directory.CreateDirectory(root); string package = Path.Combine(root, Guid.NewGuid().ToString("N") + ".hup");
            try
            {
                using (var output = new FileStream(package, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                { Download(Origin + "/releases/windows/" + manifest.sequence + ".hup", output, 256L * 1024 * 1024, cancel); output.Flush(true); }
                cancel.ThrowIfCancellationRequested(); string prepared = UpdateCoordinator.Prepare(package);
                // The downloaded package must match the metadata we checked, even if the feed changes during download.
                var staged = ReleasePackages.Verify(File.ReadAllBytes(Path.Combine(prepared, "stage", "release.json")), -1, ReleaseKey.PublicXml);
                if (staged.sequence != manifest.sequence || staged.version != manifest.version || staged.files.Length != manifest.files.Length ||
                    !manifest.files.All(f => staged.files.Any(s => s.name == f.name && s.sha256 == f.sha256 && s.size == f.size))) throw new InvalidDataException("Release changed during download.");
                return prepared;
            }
            finally { if (File.Exists(package)) File.Delete(package); }
        }
        static void Download(string url, Stream output, long maximum, CancellationToken cancel)
        {
            var request = (HttpWebRequest)WebRequest.Create(url);
            request.AllowAutoRedirect = false; request.Timeout = 5000; request.ReadWriteTimeout = 5000; request.MaximumResponseHeadersLength = 16;
            request.Method = "GET"; request.UserAgent = "Hyperlink-update";
            DateTime deadline = DateTime.UtcNow.AddMinutes(3);
            using (cancel.Register(request.Abort)) using (var response = (HttpWebResponse)request.GetResponse())
            {
                if (response.StatusCode != HttpStatusCode.OK || response.ResponseUri.Scheme != "https" || response.ResponseUri.Host != "hyperlink.myfamilyapps.ca" || response.ContentLength > maximum)
                    throw new IOException("Invalid update download response.");
                using (var input = response.GetResponseStream())
                {
                    byte[] buffer = new byte[65536]; long total = 0; int count;
                    while ((count = input.Read(buffer, 0, buffer.Length)) != 0)
                    { cancel.ThrowIfCancellationRequested(); if (DateTime.UtcNow > deadline || (total += count) > maximum) throw new IOException("Update download exceeded its bounds."); output.Write(buffer, 0, count); }
                    if (response.ContentLength >= 0 && total != response.ContentLength) throw new IOException("Truncated update download.");
                }
            }
        }
        internal static void Attach(MainWindow owner, Store store)
        {
            var cancel = new CancellationTokenSource(); var timer = new System.Windows.Forms.Timer { Interval = 60000 };
            bool busy = false; DateTime next = DateTime.MinValue;
            timer.Tick += async delegate
            {
                bool enabled; lock (store.Sync) enabled = store.Data.AutoCheckUpdates;
                if (!enabled || busy || DateTime.UtcNow < next || owner.IsDisposed) return;
                busy = true; next = DateTime.UtcNow.AddHours(24);
                try
                {
                    var release = await Task.Run(() => Latest(cancel.Token)); if (release == null || owner.IsDisposed) return;
                    bool automatic; lock (store.Sync) automatic = store.Data.AutoInstallUpdates;
                    if (automatic && owner.CanInstallAutomatically)
                    {
                        string job = await Task.Run(() => PrepareLatest(cancel.Token));
                        if (job != null && !owner.IsDisposed) owner.InstallAutomatically(job);
                    }
                    else if (Application.OpenForms.Count == 1 && MessageBox.Show(owner, "A signed Hyperlink update is available. Open the update screen?", "Hyperlink update", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
                        using (var window = new UpdateWindow(owner, store)) window.ShowDialog(owner);
                }
                catch { /* Keep automatic checks quiet while the feed is unavailable. Manual checks report errors. */ }
                finally { busy = false; }
            };
            owner.Shown += delegate { timer.Start(); };
            owner.FormClosed += delegate { timer.Dispose(); cancel.Cancel(); cancel.Dispose(); };
        }
    }
}
