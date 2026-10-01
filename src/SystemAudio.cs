using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace Hyperlink
{
    // The only capture endpoint is WASAPI loopback of the default playback device.
    // Capture callbacks never wait for network I/O. Four bounded packets limit queued audio.
    sealed class SystemAudioCapture : IDisposable
    {
        readonly BlockingCollection<byte[]> queue = new BlockingCollection<byte[]>(4);
        readonly Func<byte[], bool> send;
        readonly Guid id = Guid.NewGuid();
        readonly bool synthetic;
        readonly WaveFormat format;
        readonly object lifecycle = new object();
        WasapiLoopbackCapture capture;
        Timer timer;
        int disposed;
        volatile bool running;
        public SystemAudioCapture(bool test, Func<byte[], bool> sender)
        {
            synthetic = test; send = sender;
            if (test) format = new WaveFormat(48000, 16, 2);
            else
            {
                capture = new WasapiLoopbackCapture();
                // NAudio enables shared-mode AutoConvertPcm/SrcDefaultQuality on supported Windows.
                // Use a portable stereo stream even when the default endpoint is surround sound.
                format = new WaveFormat(48000, 16, 2);
                try { Validate(format); capture.WaveFormat = format; capture.DataAvailable += DataAvailable; }
                catch { capture.Dispose(); capture = null; throw; }
            }
        }
        static void Validate(WaveFormat value)
        {
            if (value.SampleRate < 8000 || value.SampleRate > 192000 || value.Channels < 1 || value.Channels > 2 ||
                !((value.Encoding == WaveFormatEncoding.Pcm && value.BitsPerSample == 16) || (value.Encoding == WaveFormatEncoding.IeeeFloat && value.BitsPerSample == 32)))
                throw new IOException("This draft supports mono/stereo PCM16 or float32 system playback.");
        }
        public object Describe() { return new { id = id.ToString("N"), sampleRate = format.SampleRate, channels = format.Channels, bits = format.BitsPerSample, encoding = (int)format.Encoding }; }
        public bool Owns(byte[] packet) { if (!running || packet.Length < 16) return false; var key = new byte[16]; Buffer.BlockCopy(packet, 0, key, 0, 16); return new Guid(key) == id; }
        public void Start()
        {
            lock (lifecycle) {
            if (Volatile.Read(ref disposed) != 0 || running) return; running = true;
            var worker = new Thread(delegate()
            {
                try { foreach (byte[] packet in queue.GetConsumingEnumerable()) { if (!running || !send(packet)) break; } }
                catch (Exception) { }
                finally { Dispose(); queue.Dispose(); }
            }) { IsBackground = true, Name = "Hyperlink authorized system audio" }; worker.Start();
            try
            {
                if (synthetic) timer = new Timer(delegate { Add(new byte[3840], 3840); }, null, 0, 20);
                else capture.StartRecording();
            }
            catch { Dispose(); throw; }
            }
        }
        void DataAvailable(object sender, WaveInEventArgs data) { Add(data.Buffer, data.BytesRecorded); }
        void Add(byte[] bytes, int length)
        {
            if (!running || length < 1 || length % format.BlockAlign != 0) return;
            for (int offset = 0; offset < length && running;)
            {
                int count = Math.Min(8192 - 8192 % format.BlockAlign, length - offset);
                var packet = new byte[16 + count]; Buffer.BlockCopy(id.ToByteArray(), 0, packet, 0, 16); Buffer.BlockCopy(bytes, offset, packet, 16, count);
                try { queue.TryAdd(packet); } catch (InvalidOperationException) { return; }
                offset += count;
            }
        }
        public void Dispose()
        {
            if (Interlocked.Exchange(ref disposed, 1) != 0) return; running = false;
            lock (lifecycle) {
            if (timer != null) timer.Dispose(); queue.CompleteAdding();
            if (capture != null) { try { capture.StopRecording(); } finally { capture.Dispose(); capture = null; } }
            }
        }
    }

    sealed class SystemAudioPlayback : IDisposable
    {
        readonly Guid id;
        readonly BufferedWaveProvider buffer;
        readonly WasapiOut output;
        readonly object sync = new object();
        bool disposed;
        public SystemAudioPlayback(Dictionary<string, object> descriptor)
        {
            if (!Guid.TryParseExact(Wire.Text(descriptor, "id"), "N", out id)) throw new InvalidDataException("Invalid audio stream.");
            int rate = Wire.Number(descriptor, "sampleRate"), channels = Wire.Number(descriptor, "channels"), bits = Wire.Number(descriptor, "bits"), encoding = Wire.Number(descriptor, "encoding");
            if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2 || !((encoding == 1 && bits == 16) || (encoding == 3 && bits == 32))) throw new InvalidDataException("Unsupported audio format.");
            WaveFormat format = encoding == 3 ? WaveFormat.CreateIeeeFloatWaveFormat(rate, channels) : new WaveFormat(rate, bits, channels);
            buffer = new BufferedWaveProvider(format) { BufferDuration = TimeSpan.FromMilliseconds(120), DiscardOnBufferOverflow = true, ReadFully = true };
            output = new WasapiOut(AudioClientShareMode.Shared, 60);
            try { output.Init(buffer); output.Play(); } catch { output.Dispose(); throw; }
        }
        public void Add(byte[] packet)
        {
            if (packet.Length < 17 || packet.Length > 8208) throw new InvalidDataException("Invalid audio packet.");
            var key = new byte[16]; Buffer.BlockCopy(packet, 0, key, 0, 16); if (new Guid(key) != id) return;
            if ((packet.Length - 16) % buffer.WaveFormat.BlockAlign != 0) throw new InvalidDataException("Unaligned audio packet.");
            lock (sync) if (!disposed) buffer.AddSamples(packet, 16, packet.Length - 16);
        }
        public void Dispose() { lock (sync) { if (disposed) return; disposed = true; } try { output.Stop(); } finally { output.Dispose(); } }
    }
}
