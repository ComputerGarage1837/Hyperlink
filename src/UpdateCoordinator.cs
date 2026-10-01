using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Win32;

namespace Hyperlink
{
    sealed class UpdateJob
    {
        public string Install { get; set; }
        public string Phase { get; set; }
        public int Parent { get; set; }
        public int Worker { get; set; }
        public long LaunchedUtcTicks { get; set; }
        public Dictionary<string, string> Original { get; set; }
    }
    static class UpdateCoordinator
    {
        const string RegistryPath = "Software\\Hyperlink\\Updates";
        static readonly string Root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Hyperlink", "Updates");
        static string healthJob;
        internal static int Floor
        {
            get { using (var key = Registry.CurrentUser.OpenSubKey(RegistryPath)) { object value = key == null ? null : key.GetValue("Sequence"); if (value == null) return 0; if (!(value is int) || (int)value < 0) throw new IOException("Invalid update rollback state."); return (int)value; } }
        }
        static string Pending
        {
            get { using (var key = Registry.CurrentUser.OpenSubKey(RegistryPath)) { object value = key == null ? null : key.GetValue("Pending"); if (value == null) return null; if (!(value is string)) throw new IOException("Invalid update recovery state."); return (string)value; } }
        }
        static void SetPending(string path)
        {
            using (var key = Registry.CurrentUser.CreateSubKey(RegistryPath)) { if (path == null) key.DeleteValue("Pending", false); else key.SetValue("Pending", path, RegistryValueKind.String); key.Flush(); }
        }
        static void SetFloor(int sequence)
        {
            if (sequence < Floor) throw new IOException("Update rollback refused.");
            using (var key = Registry.CurrentUser.CreateSubKey(RegistryPath)) { key.SetValue("Sequence", sequence, RegistryValueKind.DWord); key.Flush(); }
            if (Floor != sequence) throw new IOException("Update rollback state could not be saved.");
        }
        static void DirectoryCheck(string path)
        {
            string full = Path.GetFullPath(path);
            if (full.StartsWith("\\\\", StringComparison.Ordinal) || new DriveInfo(Path.GetPathRoot(full)).DriveType == DriveType.Network) throw new IOException("Updates require local storage.");
            for (var directory = new DirectoryInfo(full); directory != null; directory = directory.Parent)
                if (directory.Exists && (directory.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("Update paths cannot contain links or junctions.");
        }
        static string JobPath(string path)
        {
            string full = Path.GetFullPath(path), root = Path.GetFullPath(Root).TrimEnd(Path.DirectorySeparatorChar);
            if (!string.Equals(Path.GetDirectoryName(full), root, StringComparison.OrdinalIgnoreCase) ||
                !System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(full), "\\A[a-f0-9]{32}\\z")) throw new IOException("Invalid update job path.");
            DirectoryCheck(full); return full;
        }
        static UpdateJob Load(string path)
        {
            path = JobPath(path); string journal = Path.Combine(path, "job.json");
            if (new FileInfo(journal).Length > 8192) throw new IOException("Invalid update journal size.");
            var job = new JavaScriptSerializer { MaxJsonLength = 8192, RecursionLimit = 8 }.Deserialize<UpdateJob>(File.ReadAllText(journal));
            if (job == null || job.Install == null || !Path.IsPathRooted(job.Install) || job.Parent < 1 || job.Worker < 0 ||
                !new[] { "staged", "prepared", "installing", "committing", "complete", "restored" }.Contains(job.Phase)) throw new IOException("Invalid update journal.");
            job.Install = Path.GetFullPath(job.Install); DirectoryCheck(job.Install);
            if (!File.Exists(Path.Combine(job.Install, "Hyperlink.exe"))) throw new IOException("Update installation is missing.");
            if (job.Original != null && job.Original.Any(p => !ReleasePackages.AllowedNames.Contains(p.Key, StringComparer.Ordinal) || p.Value == null || !System.Text.RegularExpressions.Regex.IsMatch(p.Value, "\\A[a-f0-9]{64}\\z"))) throw new IOException("Invalid update backup inventory.");
            return job;
        }
        static void Save(string path, UpdateJob job)
        {
            string journal = Path.Combine(JobPath(path), "job.json"), temporary = journal + ".tmp";
            byte[] bytes = Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(job));
            using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None)) { output.Write(bytes, 0, bytes.Length); output.Flush(true); }
            if (File.Exists(journal)) File.Replace(temporary, journal, null); else File.Move(temporary, journal);
        }
        static ReleaseManifest Manifest(string path, int floor)
        {
            string stage = Path.Combine(JobPath(path), "stage");
            if (new FileInfo(Path.Combine(stage, "release.json")).Length > 32768) throw new IOException("Invalid staged release manifest size.");
            var manifest = ReleasePackages.Verify(File.ReadAllBytes(Path.Combine(stage, "release.json")), floor, ReleaseKey.PublicXml);
            ReleasePackages.VerifyFiles(stage, manifest); return manifest;
        }
        internal static string Prepare(string package)
        {
            foreach (var process in Process.GetProcessesByName("Hyperlink")) using (process)
                if (process.Id != Process.GetCurrentProcess().Id && !process.HasExited && string.Equals(process.MainModule.FileName, Path.Combine(Application.StartupPath, "Hyperlink.exe"), StringComparison.OrdinalIgnoreCase))
                    throw new IOException("Close the other Hyperlink process in this installation before updating.");
            DirectoryCheck(Application.StartupPath); Directory.CreateDirectory(Root); DirectoryCheck(Root);
            string path = Path.Combine(Root, Guid.NewGuid().ToString("N")); Directory.CreateDirectory(path);
            var manifest = ReleasePackages.Stage(package, Path.Combine(path, "stage"), Floor);
            if (Version.Parse(manifest.version) <= System.Reflection.Assembly.GetExecutingAssembly().GetName().Version) throw new IOException("This release is not newer than the running app.");
            Save(path, new UpdateJob { Install = Application.StartupPath, Parent = Process.GetCurrentProcess().Id, Worker = 0, Phase = "staged" }); return path;
        }
        internal static string Describe(string path) { var manifest = Manifest(path, Floor); return "Hyperlink " + manifest.version + " · release " + manifest.sequence; }
        internal static void Start(string path)
        {
            var job = Load(path); Manifest(path, Floor);
            if (!string.Equals(job.Install, Application.StartupPath, StringComparison.OrdinalIgnoreCase) || Pending != null) throw new IOException("Another update is pending or the installation changed.");
            LaunchWorker(path, job, false);
        }
        static void LaunchWorker(string path, UpdateJob job, bool recover)
        {
            path = JobPath(path); string helper = Path.Combine(path, "helper"); Directory.CreateDirectory(helper); DirectoryCheck(helper);
            foreach (string name in new[] { "Hyperlink.exe", "NAudio.Core.dll", "NAudio.Wasapi.dll" })
                File.Copy(Path.Combine(Application.StartupPath, name), Path.Combine(helper, name), true);
            job.Parent = Process.GetCurrentProcess().Id; job.Worker = 0; job.LaunchedUtcTicks = DateTime.UtcNow.Ticks; Save(path, job); SetPending(path);
            try { using (Process process = Process.Start(new ProcessStartInfo(Path.Combine(helper, "Hyperlink.exe"), (recover ? "--update-recover" : "--update-apply") + " \"" + path + "\"") { UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = helper })) { if (process == null) throw new IOException("Update helper did not start."); job.Worker = process.Id; Save(path, job); } }
            catch { if (!recover) SetPending(null); throw; }
        }
        static bool IsWorkerAlive(string path, UpdateJob job)
        {
            if (job.Worker < 1) return job.LaunchedUtcTicks > 0 && DateTime.UtcNow.Ticks - job.LaunchedUtcTicks < TimeSpan.FromSeconds(30).Ticks;
            try { using (var process = Process.GetProcessById(job.Worker)) return !process.HasExited && string.Equals(process.MainModule.FileName, Path.Combine(path, "helper", "Hyperlink.exe"), StringComparison.OrdinalIgnoreCase); }
            catch (ArgumentException) { return false; }
        }
        internal static int? Startup(string[] args)
        {
            if (args.Length == 2 && (args[0] == "--update-apply" || args[0] == "--update-recover")) return Work(args[1], args[0] == "--update-recover");
            if (args.Length == 2 && args[0] == "--update-health")
            {
                string path = JobPath(args[1]); var job = Load(path);
                if (Pending != path || job.Phase != "installing" || !IsWorkerAlive(path, job) || !string.Equals(job.Install, Application.StartupPath, StringComparison.OrdinalIgnoreCase)) throw new IOException("Unexpected update health check.");
                var manifest = Manifest(path, Floor); ReleasePackages.VerifyFiles(job.Install, manifest); healthJob = path; return null;
            }
            string pending = Pending;
            if (pending == null) return null;
            pending = JobPath(pending); var existing = Load(pending);
            if (!string.Equals(existing.Install, Application.StartupPath, StringComparison.OrdinalIgnoreCase)) throw new IOException("An update for another Hyperlink installation is pending.");
            if (IsWorkerAlive(pending, existing)) { MessageBox.Show("Hyperlink is finishing an update. Please wait before opening it again.", "Hyperlink update"); return 0; }
            LaunchWorker(pending, existing, true); return 0;
        }
        internal static void MarkHealthy() { if (healthJob != null) File.WriteAllText(Path.Combine(healthJob, "healthy"), "ready"); }
        static void WaitParent(UpdateJob job)
        {
            try { using (var parent = Process.GetProcessById(job.Parent)) { if (!string.Equals(parent.MainModule.FileName, Path.Combine(job.Install, "Hyperlink.exe"), StringComparison.OrdinalIgnoreCase)) throw new IOException("Update parent identity changed."); if (!parent.WaitForExit(20000)) throw new IOException("Hyperlink did not close for the update."); } }
            catch (ArgumentException) { }
        }
        static void LaunchApp(string install, string arguments)
        { using (var process = Process.Start(new ProcessStartInfo(Path.Combine(install, "Hyperlink.exe"), arguments) { UseShellExecute = false, WorkingDirectory = install })) { if (process == null) throw new IOException("Hyperlink did not restart."); } }
        static int Work(string path, bool recover)
        {
            Process restarted = null; UpdateJob job = null; bool ownsJob = false;
            try
            {
                path = JobPath(path); job = Load(path);
                if (Pending != path || !string.Equals(Application.StartupPath, Path.Combine(path, "helper"), StringComparison.OrdinalIgnoreCase)) throw new IOException("Unexpected update helper.");
                ownsJob = true;
                job.Worker = Process.GetCurrentProcess().Id; Save(path, job); WaitParent(job);
                var manifest = Manifest(path, -1);
                if (recover)
                {
                    if (job.Phase == "committing" || Floor >= manifest.sequence) { ReleasePackages.VerifyFiles(job.Install, manifest); if (Floor < manifest.sequence) SetFloor(manifest.sequence); job.Phase = "complete"; }
                    else if (job.Phase == "installing") { if (job.Original == null) throw new IOException("Recovery inventory missing."); UpdateTransaction.Restore(job.Install, Path.Combine(path, "backup"), job.Original); job.Phase = "restored"; }
                    Save(path, job); SetPending(null); LaunchApp(job.Install, ""); return 0;
                }
                Manifest(path, Floor);
                job.Original = UpdateTransaction.Backup(job.Install, Path.Combine(path, "backup")); job.Phase = "prepared"; Save(path, job);
                job.Phase = "installing"; Save(path, job);
                UpdateTransaction.Apply(job.Install, Path.Combine(path, "stage"), manifest, null);
                string health = Path.Combine(path, "healthy"); if (File.Exists(health)) File.Delete(health);
                restarted = Process.Start(new ProcessStartInfo(Path.Combine(job.Install, "Hyperlink.exe"), "--update-health \"" + path + "\"") { UseShellExecute = false, WorkingDirectory = job.Install });
                if (restarted == null) throw new IOException("Updated app did not start.");
                DateTime deadline = DateTime.UtcNow.AddSeconds(20);
                while (!File.Exists(health)) { if (restarted.HasExited || DateTime.UtcNow > deadline) throw new IOException("Updated app did not become ready."); Thread.Sleep(100); }
                ReleasePackages.VerifyFiles(job.Install, manifest); job.Phase = "committing"; Save(path, job);
                SetFloor(manifest.sequence); job.Phase = "complete"; Save(path, job); SetPending(null); return 0;
            }
            catch
            {
                if (!ownsJob) return 1;
                if (job.Phase == "committing" || job.Phase == "complete")
                { MessageBox.Show("The new version is installed, but its update state needs recovery. Keep the update folder; the next launch will finish verification.", "Hyperlink update", MessageBoxButtons.OK, MessageBoxIcon.Warning); return 1; }
                try
                {
                    if (restarted != null && !restarted.HasExited)
                    {
                        if (!string.Equals(restarted.MainModule.FileName, Path.Combine(job.Install, "Hyperlink.exe"), StringComparison.OrdinalIgnoreCase)) throw new IOException("Update child identity changed.");
                        restarted.CloseMainWindow(); if (!restarted.WaitForExit(5000)) { restarted.Kill(); restarted.WaitForExit(5000); }
                    }
                    if (job != null && job.Phase == "installing" && job.Original != null)
                    { UpdateTransaction.Restore(job.Install, Path.Combine(path, "backup"), job.Original); job.Phase = "restored"; Save(path, job); }
                    SetPending(null); if (job != null) LaunchApp(job.Install, "");
                    MessageBox.Show("The update could not finish. Hyperlink kept or restored the previous version. Your identity data was not replaced.", "Hyperlink update", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                catch { MessageBox.Show("Update recovery needs attention. Keep the update backup and do not delete the pending update folder.", "Hyperlink update", MessageBoxButtons.OK, MessageBoxIcon.Error); }
                return 1;
            }
            finally { if (restarted != null) restarted.Dispose(); }
        }
    }
    sealed class UpdateWindow : Form
    {
        readonly Label status; readonly Button choose, install, online; string job;
        readonly CancellationTokenSource download = new CancellationTokenSource();
        internal UpdateWindow(Form owner, Store store)
        {
            Text = "Verified updates"; Size = new System.Drawing.Size(620, 410); Font = Theme.Font(10); StartPosition = FormStartPosition.CenterParent;
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(16), ColumnCount = 1, RowCount = 5 };
            body.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); for (int row = 0; row < 3; row++) body.RowStyles.Add(new RowStyle(SizeType.Absolute, 44)); body.RowStyles.Add(new RowStyle(SizeType.Absolute, 70)); Controls.Add(body);
            status = new Label { Dock = DockStyle.Fill, Text = "Choose a .hup package signed by Hyperlink's offline release key. The installer verifies every public file, preserves Data, and retains a recovery backup. Automatic downloads are available from your private release feed. Windows installs when the app and sessions are idle." };
            choose = new Button { Text = "Choose update package", Dock = DockStyle.Fill }; install = new Button { Text = "Install verified update and restart", Dock = DockStyle.Fill, Enabled = false };
            online = new Button { Text = "Check release feed", Dock = DockStyle.Fill };
            body.Controls.Add(status, 0, 0); body.Controls.Add(choose, 0, 1); body.Controls.Add(online, 0, 2); body.Controls.Add(install, 0, 3);
            var preferences = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false };
            var check = new CheckBox { Text = "Check for signed updates every six hours", AutoSize = true };
            var automatic = new CheckBox { Text = "Install automatically when the app and sessions are idle", AutoSize = true };
            lock (store.Sync) { check.Checked = store.Data.AutoCheckUpdates; automatic.Checked = store.Data.AutoInstallUpdates; } automatic.Enabled = check.Checked;
            preferences.Controls.Add(check); preferences.Controls.Add(automatic); body.Controls.Add(preferences, 0, 4);
            bool saving = false;
            Action savePreferences = delegate
            {
                if (saving) return; saving = true;
                lock (store.Sync)
                {
                    bool oldCheck = store.Data.AutoCheckUpdates, oldInstall = store.Data.AutoInstallUpdates;
                    try { store.Data.AutoCheckUpdates = check.Checked; store.Data.AutoInstallUpdates = automatic.Checked; store.Save(); }
                    catch { store.Data.AutoCheckUpdates = oldCheck; store.Data.AutoInstallUpdates = oldInstall; check.Checked = oldCheck; automatic.Checked = oldInstall; status.Text = "Update preferences could not be saved."; }
                }
                automatic.Enabled = check.Checked; saving = false;
            };
            check.CheckedChanged += delegate { savePreferences(); }; automatic.CheckedChanged += delegate { savePreferences(); };
            FormClosed += delegate { download.Cancel(); download.Dispose(); };
            online.Click += async delegate
            {
                choose.Enabled = online.Enabled = install.Enabled = false; status.Text = "Checking signed release metadata…"; CancellationToken token = download.Token;
                try { string prepared = await System.Threading.Tasks.Task.Run(() => UpdateFeed.PrepareLatest(token)); if (IsDisposed) return; job = prepared; status.Text = job == null ? "No newer signed release is available." : "Verified: " + UpdateCoordinator.Describe(job) + "\r\nReady to install. Your current session will close."; install.Enabled = job != null; }
                catch { if (!IsDisposed) status.Text = "The release feed is unavailable or the release could not be verified. Local signed packages still work."; }
                finally { if (!IsDisposed) choose.Enabled = online.Enabled = true; }
            };
            choose.Click += async delegate
            {
                using (var picker = new OpenFileDialog { Filter = "Signed Hyperlink release|*.hup", CheckFileExists = true })
                {
                    if (picker.ShowDialog(this) != DialogResult.OK) return; string package = picker.FileName; choose.Enabled = online.Enabled = false; install.Enabled = false; status.Text = "Verifying package…";
                    try { string prepared = await System.Threading.Tasks.Task.Run(() => UpdateCoordinator.Prepare(package)); if (IsDisposed) return; job = prepared; status.Text = "Verified: " + UpdateCoordinator.Describe(job) + "\r\nReady to install. Your current session will close."; install.Enabled = true; }
                    catch { if (!IsDisposed) status.Text = "Package refused. It may be unsigned, damaged, older, or incompatible with this draft."; }
                    finally { if (!IsDisposed) choose.Enabled = online.Enabled = true; }
                }
            };
            install.Click += delegate { if (JpegRecording.HasActive) { status.Text = "Stop and save active recordings before installing the update."; return; } try { UpdateCoordinator.Start(job); Close(); owner.Close(); } catch { status.Text = "Could not start the update. No installed files were changed."; } };
        }
    }
}
