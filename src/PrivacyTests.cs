using System;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace Hyperlink
{
    static class PrivacyTests
    {
        [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr context);
        [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr window, int command);
        internal static int ScreenCheck(string report)
        {
            SetProcessDpiAwarenessContext(new IntPtr(-4)); Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            int result = 1, phase = 0, escaped = 0; PrivacyScreen privacy = null;
            using (var under = new Form { FormBorderStyle = FormBorderStyle.None, StartPosition = FormStartPosition.CenterScreen, Size = new Size(240, 240), BackColor = Color.Teal, TopMost = true, ShowInTaskbar = false })
            using (var timer = new System.Windows.Forms.Timer { Interval = 650 })
            {
                timer.Tick += delegate
                {
                    try
                    {
                        if (phase++ == 0) { ShowWindow(under.Handle, 5); under.Activate(); under.Refresh(); return; }
                        if (phase == 2)
                        {
                            if (Pixel(under).ToArgb() != Color.Teal.ToArgb()) throw new Exception("The owned test surface was not visible; the privacy check is inconclusive.");
                            privacy = new PrivacyScreen(delegate { Interlocked.Exchange(ref escaped, 1); }, new[] { under.Bounds }); return;
                        }
                        if (phase == 3)
                        {
                            if (Pixel(under).ToArgb() != Color.Teal.ToArgb()) throw new Exception("Privacy cover obscured the captured owned surface.");
                            privacy.TestEmergencyMessage(); return;
                        }
                        if (Volatile.Read(ref escaped) == 0 || privacy.Active || !privacy.Completion.IsCompleted) throw new Exception("Emergency recovery did not finish.");
                        File.WriteAllText(report, "PASS: the owned surface stayed visible to GDI capture beneath the excluded privacy cover; the registered hotkey message restored the display and invoked disconnect. Only a small owned test window was covered; full multi-monitor privacy remains a pilot check."); result = 0;
                    }
                    catch (Exception ex) { phase = 4; File.WriteAllText(report, "Privacy check failed: " + ex.Message); }
                    finally { if (phase >= 4 || result == 0) { timer.Stop(); if (privacy != null) privacy.Dispose(); under.Close(); } }
                };
                under.Shown += delegate { timer.Start(); }; Application.Run(under);
                if (privacy != null) { privacy.Dispose(); privacy.Completion.Wait(5000); }
            }
            return result;
        }
        static Color Pixel(Form under)
        {
            using (var bitmap = new Bitmap(1, 1)) using (var graphics = Graphics.FromImage(bitmap))
            { graphics.CopyFromScreen(under.Left + 120, under.Top + 120, 0, 0, new Size(1, 1), CopyPixelOperation.SourceCopy); return bitmap.GetPixel(0, 0); }
        }
    }
}
