using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace Hyperlink
{
    static class VideoTests
    {
        internal static int Run(string report)
        {
            try
            {
                int frames = 0, changes = 0, previous = 0, failed = 0; int width = 1920, height = 1080;
                long firstTick = 0, warmTick = 0, lastTick = 0;
                long sourceWait = 0, queueWait = 0, bitmapTime = 0;
                var clock = Stopwatch.StartNew();
                using (var decoder = new NativeVideoDecoder(width, height, 120, delegate(System.Drawing.Bitmap image)
                {
                    long callbackStart = Stopwatch.GetTimestamp();
                    try
                    {
                        if (image.Width != width || image.Height != height) throw new Exception("Decoded dimensions changed.");
                        // Sample generated moving-pattern pixels; no desktop content is captured or persisted.
                        int sample = 0; for (int y = 40; y < height; y += 173) for (int x = 20; x < width; x += 191) sample = unchecked(sample * 31 + image.GetPixel(x, y).ToArgb());
                        if (frames == 0 || sample != previous) Interlocked.Increment(ref changes); previous = sample; int number = Interlocked.Increment(ref frames);
                        long tick = Stopwatch.GetTimestamp(); if (number == 1) firstTick = tick; if (number == 121) warmTick = tick; lastTick = tick;
                    }
                    finally { image.Dispose(); Interlocked.Add(ref bitmapTime, Stopwatch.GetTimestamp() - callbackStart); }
                }, delegate { Interlocked.Exchange(ref failed, 1); }))
                using (var encoder = new NativeVideoEncoder(0, width, height, 120, true))
                {
                    decoder.Accept(encoder.Next()); DateTime firstDeadline = DateTime.UtcNow.AddSeconds(2);
                    while (Volatile.Read(ref frames) == 0 && Volatile.Read(ref failed) == 0 && DateTime.UtcNow < firstDeadline) Thread.Sleep(10);
                    if (Volatile.Read(ref frames) == 0) throw new Exception("A single static frame did not become visible without another frame.");
                    for (int i = 1; i < 360; i++) { if (Volatile.Read(ref failed) != 0) throw new Exception("Native decoding failed."); long begin = Stopwatch.GetTimestamp(); byte[] packet = encoder.Next(); sourceWait += Stopwatch.GetTimestamp() - begin; begin = Stopwatch.GetTimestamp(); decoder.Accept(packet); queueWait += Stopwatch.GetTimestamp() - begin; }
                    DateTime deadline = DateTime.UtcNow.AddSeconds(5);
                    while (Volatile.Read(ref frames) < 360 && Volatile.Read(ref failed) == 0 && DateTime.UtcNow < deadline) Thread.Sleep(20);
                    if (Volatile.Read(ref failed) != 0 || frames != 360 || changes < 350) throw new Exception("Generated frames were lost, repeated, or did not decode.");
                }
                double warmRate = 239.0 * Stopwatch.Frequency / (lastTick - warmTick);
                string stages = "Average source wait: " + (sourceWait * 1000.0 / Stopwatch.Frequency / 359).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + " ms\r\nAverage decoder queue wait: " + (queueWait * 1000.0 / Stopwatch.Frequency / 359).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + " ms\r\nAverage bitmap sample/disposal: " + (bitmapTime * 1000.0 / Stopwatch.Frequency / 360).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + " ms\r\n";
                File.WriteAllText(report, "1080p generated H.264 frames decoded: " + frames + "\r\nChanging sampled frames: " + changes + "\r\nWarm generated-frame rate: " + warmRate.ToString("F1", System.Globalization.CultureInfo.InvariantCulture) + " fps\r\nFirst-to-last presentation: " + ((lastTick - firstTick) / (double)Stopwatch.Frequency).ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + " seconds\r\nElapsed including codec startup: " + clock.Elapsed.TotalSeconds.ToString("F2", System.Globalization.CultureInfo.InvariantCulture) + " seconds\r\n" + stages + "This is a short synthetic codec check, not 120 FPS desktop qualification.\r\n"); return 0;
            }
            catch (Exception e) { File.WriteAllText(report, "Video check failed: " + e.Message); return 1; }
        }
    }
}
