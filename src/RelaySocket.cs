using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace Hyperlink
{
    sealed class RelaySocket : Stream
    {
        internal const string Endpoint = "wss://hyperlink.myfamilyapps.ca/relay/";
        readonly ClientWebSocket socket = new ClientWebSocket();
        readonly CancellationTokenSource lifetime = new CancellationTokenSource();
        readonly object readSync = new object(), writeSync = new object();
        byte[] pending; int position;
        int readTimeout = 45000, writeTimeout = 15000;
        RelaySocket() { socket.Options.KeepAliveInterval = TimeSpan.FromSeconds(15); }
        internal static RelaySocket Open(string path)
        {
            if (path != "control" && path != "session") throw new ArgumentException("Invalid relay endpoint.");
            var result = new RelaySocket();
            try
            {
                using (var timeout = result.Timeout(15000)) result.socket.ConnectAsync(new Uri(Endpoint + path), timeout.Token).GetAwaiter().GetResult();
                return result;
            }
            catch { result.Dispose(); throw; }
        }
        CancellationTokenSource Timeout(int milliseconds)
        {
            var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token); timeout.CancelAfter(milliseconds); return timeout;
        }
        internal void SendJson(object value)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(value));
            if (bytes.Length > 16384) throw new IOException("Relay request too large.");
            lock (writeSync) using (var timeout = Timeout(writeTimeout)) socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, timeout.Token).GetAwaiter().GetResult();
        }
        byte[] Receive(WebSocketMessageType expected, int limit)
        {
            using (var timeout = Timeout(readTimeout)) using (var bytes = new MemoryStream())
            {
                byte[] buffer = new byte[Math.Min(limit, 65536)]; WebSocketReceiveResult result;
                do
                {
                    result = socket.ReceiveAsync(new ArraySegment<byte>(buffer), timeout.Token).GetAwaiter().GetResult();
                    if (result.MessageType != expected || bytes.Length + result.Count > limit) throw new IOException("Relay connection ended or sent invalid data.");
                    bytes.Write(buffer, 0, result.Count);
                } while (!result.EndOfMessage);
                if (bytes.Length == 0) throw new IOException("Empty relay packet.");
                return bytes.ToArray();
            }
        }
        internal Dictionary<string, object> ReadJson()
        {
            lock (readSync)
            {
                var serializer = new JavaScriptSerializer { MaxJsonLength = 16384, RecursionLimit = 8 };
                var value = serializer.DeserializeObject(new UTF8Encoding(false, true).GetString(Receive(WebSocketMessageType.Text, 16384))) as Dictionary<string, object>;
                if (value == null || Wire.Text(value, "kind") == "error") throw new IOException("Computer unavailable or relay request refused.");
                return value;
            }
        }
        internal static RelaySocket Tunnel(Dictionary<string, object> details)
        {
            var socket = Open("session");
            try
            {
                socket.SendJson(new { session = Wire.Text(details, "session"), token = Wire.Text(details, "token") });
                if (Wire.Text(socket.ReadJson(), "kind") != "ready") throw new IOException("Relay tunnel was not accepted.");
                return socket;
            }
            catch { socket.Dispose(); throw; }
        }
        internal static Dictionary<string, object> Lookup(string code)
        {
            if (!System.Text.RegularExpressions.Regex.IsMatch(code ?? "", "^[1-9][0-9]{7}$")) throw new ArgumentException("Enter the 8-digit computer ID.");
            using (var socket = Open("control"))
            {
                socket.SendJson(new { operation = "lookup", code = code }); var target = socket.ReadJson();
                if (Wire.Text(target, "kind") != "computer" || Wire.Text(target, "code") != code) throw new IOException("Computer unavailable.");
                foreach (string field in new[] { "fingerprint", "id" }) if (!System.Text.RegularExpressions.Regex.IsMatch(Wire.Text(target, field), "^[a-f0-9]{64}$")) throw new IOException("Invalid computer identity.");
                if (!HostAuthority.Verify(Wire.Text(target, "id"), Wire.Text(target, "fingerprint"), code, Wire.Text(target, "attestation"))) throw new IOException("This computer's identity could not be verified.");
                return target;
            }
        }
        internal static Wire Secure(RelaySocket transport, string fingerprint)
        {
            try
            {
                var ssl = new SslStream(transport, false, delegate(object sender, X509Certificate cert, X509Chain chain, SslPolicyErrors errors) {
                    if (cert == null || !Util.Equal(Util.Hash(cert.GetRawCertData()), fingerprint)) return false;
                    using (var value = new X509Certificate2(cert)) return DateTime.UtcNow >= value.NotBefore.ToUniversalTime() && DateTime.UtcNow <= value.NotAfter.ToUniversalTime();
                });
                ssl.ReadTimeout = 15000; ssl.WriteTimeout = 10000; ssl.AuthenticateAsClient("Hyperlink", null, SslProtocols.Tls12, false);
                return new Wire(null, ssl);
            }
            catch { transport.Dispose(); throw; }
        }
        internal static Wire Connect(Device device)
        {
            var target = Lookup(device.RelayCode);
            if (!Util.Equal(Wire.Text(target, "fingerprint"), device.Fingerprint) || !Util.Equal(Wire.Text(target, "id"), device.Id)) throw new IOException("This computer's identity changed. Set it up again from Windows.");
            return Secure(Tunnel(target), device.Fingerprint);
        }
        public override int Read(byte[] buffer, int offset, int count)
        {
            if (count == 0) return 0;
            lock (readSync)
            {
                if (pending == null || position == pending.Length) { pending = Receive(WebSocketMessageType.Binary, 65536); position = 0; }
                int amount = Math.Min(count, pending.Length - position); Buffer.BlockCopy(pending, position, buffer, offset, amount); position += amount; return amount;
            }
        }
        public override void Write(byte[] buffer, int offset, int count)
        {
            lock (writeSync) using (var timeout = Timeout(writeTimeout))
                while (count > 0) { int amount = Math.Min(65536, count); socket.SendAsync(new ArraySegment<byte>(buffer, offset, amount), WebSocketMessageType.Binary, true, timeout.Token).GetAwaiter().GetResult(); offset += amount; count -= amount; }
        }
        protected override void Dispose(bool disposing) { if (disposing) { lifetime.Cancel(); socket.Abort(); socket.Dispose(); } base.Dispose(disposing); }
        public override bool CanRead { get { return true; } } public override bool CanWrite { get { return true; } } public override bool CanSeek { get { return false; } }
        public override bool CanTimeout { get { return true; } }
        public override int ReadTimeout { get { return readTimeout; } set { readTimeout = value; } }
        public override int WriteTimeout { get { return writeTimeout; } set { writeTimeout = value; } }
        public override void Flush() { }
        public override long Length { get { throw new NotSupportedException(); } } public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
        public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); } public override void SetLength(long value) { throw new NotSupportedException(); }
    }
    public sealed partial class Host
    {
        internal void AcceptRelay(RelaySocket transport)
        {
            TcpListener server;
            lock (gate) { server = listener; if (server == null || workers >= 4) { transport.Dispose(); return; } workers++; }
            try { new Thread(delegate() { Handle(null, server, transport); }) { IsBackground = true, Name = "Hyperlink relay session" }.Start(); }
            catch { lock (gate) workers--; transport.Dispose(); throw; }
        }
    }
}
