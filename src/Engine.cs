using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using System.Xml;

namespace Hyperlink
{
    public sealed class Peer
    {
        public string Id, Name, PublicKey;
        public bool Control;
        public bool FileRead, FileWrite, ClipboardToHost, ClipboardFromHost;
        public bool Audio;
    }
    public sealed class Device
    {
        public string Id, Name, Address, Fingerprint;
        public int Port;
        public bool Control;
    }
    public sealed class Settings
    {
        public string Name = Environment.MachineName;
        public string PrivateKey, Certificate;
        public string AccountRefresh;
        public string FamilyDeviceId;
        public int Port = 45831;
        public string SharedFolder;
        public bool AutoCheckUpdates, AutoInstallUpdates;
        public List<Peer> Peers = new List<Peer>();
        public List<Device> Devices = new List<Device>();
    }

    public sealed class Store : IDisposable
    {
        public readonly object Sync = new object();
        public readonly Settings Data;
        public readonly RSACryptoServiceProvider Key;
        public readonly X509Certificate2 Certificate;
        public readonly string Fingerprint, PublicKey, Id;
        readonly string path;
        static readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 1024 * 1024 };

        public Store(string directory)
        {
            Directory.CreateDirectory(directory);
            path = Path.Combine(directory, "identity.dat");
            if (File.Exists(path))
            {
                byte[] plain = ProtectedData.Unprotect(File.ReadAllBytes(path), null, DataProtectionScope.CurrentUser);
                Data = json.Deserialize<Settings>(Encoding.UTF8.GetString(plain));
                if (Data == null || Data.PrivateKey == null || Data.Certificate == null) throw new InvalidDataException("Invalid identity file.");
            }
            else
            {
                Data = new Settings();
                using (var rsa = new RSACryptoServiceProvider(2048))
                {
                    rsa.PersistKeyInCsp = false;
                    Data.PrivateKey = rsa.ToXmlString(true);
                    var request = new CertificateRequest("CN=Hyperlink", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
                    request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
                    request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, true));
                    var usages = new OidCollection(); usages.Add(new Oid("1.3.6.1.5.5.7.3.1"));
                    request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(usages, true));
                    using (var cert = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddYears(2)))
                        Data.Certificate = Convert.ToBase64String(cert.Export(X509ContentType.Pfx, ""));
                }
                Save();
            }
            Key = new RSACryptoServiceProvider(); Key.PersistKeyInCsp = false; Key.FromXmlString(Data.PrivateKey);
            PublicKey = Key.ToXmlString(false); Id = Util.Hash(PublicKey);
            // .NET Framework's Windows Schannel requires a temporary user key container.
            // The durable PFX remains DPAPI-protected; disposing this certificate removes the container.
            Certificate = new X509Certificate2(Convert.FromBase64String(Data.Certificate), "", X509KeyStorageFlags.UserKeySet);
            Fingerprint = Util.Hash(Certificate.RawData);
        }
        public void Save()
        {
            lock (Sync)
            {
                byte[] plain = Encoding.UTF8.GetBytes(json.Serialize(Data));
                byte[] encrypted = ProtectedData.Protect(plain, null, DataProtectionScope.CurrentUser);
                string temp = path + ".tmp";
                using (var f = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
                { f.Write(encrypted, 0, encrypted.Length); f.Flush(true); }
                if (File.Exists(path)) File.Replace(temp, path, null); else File.Move(temp, path);
            }
        }
        public string Sign(byte[] transcript)
        {
            lock (Key) return Convert.ToBase64String(Key.SignData(transcript, CryptoConfig.MapNameToOID("SHA256")));
        }
        public void Dispose() { Key.Dispose(); Certificate.Dispose(); }
    }

    public static class Util
    {
        public static string Hash(string value) { return Hash(Encoding.UTF8.GetBytes(value)); }
        public static string Hash(byte[] value) { using (var h = SHA256.Create()) return BitConverter.ToString(h.ComputeHash(value)).Replace("-", "").ToLowerInvariant(); }
        public static string Random(int bytes)
        {
            var b = new byte[bytes]; using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(b);
            return Convert.ToBase64String(b).TrimEnd('=').Replace('+', '-').Replace('/', '_');
        }
        public static byte[] Transcript(string nonce, string fingerprint, string id)
        { return Encoding.UTF8.GetBytes("Hyperlink/1\n" + nonce + "\n" + fingerprint + "\n" + id); }
        public static bool Equal(string a, string b)
        {
            if (a == null || b == null || a.Length != b.Length) return false;
            int d = 0; for (int i = 0; i < a.Length; i++) d |= a[i] ^ b[i]; return d == 0;
        }
        public static string Name(string name)
        { if (String.IsNullOrWhiteSpace(name) || name.Length > 60 || name.Any(Char.IsControl)) throw new InvalidDataException("Name must contain 1–60 printable characters."); return name.Trim(); }
        public static bool Verify(string publicKey, byte[] transcript, string signature)
        {
            if (publicKey == null || publicKey.Length > 4096 || signature == null || signature.Length > 1024) return false;
            try
            {
                var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
                var doc = new XmlDocument { XmlResolver = null };
                using (var r = XmlReader.Create(new StringReader(publicKey), settings)) doc.Load(r);
                if (doc.DocumentElement.Name != "RSAKeyValue" || doc.DocumentElement.ChildNodes.Count != 2) return false;
                var m = doc.DocumentElement.SelectSingleNode("Modulus"); var e = doc.DocumentElement.SelectSingleNode("Exponent");
                if (m == null || e == null) return false;
                byte[] modulus = Convert.FromBase64String(m.InnerText), exponent = Convert.FromBase64String(e.InnerText);
                if (modulus.Length < 256 || modulus.Length > 512 || exponent.Length < 1 || exponent.Length > 4) return false;
                using (var rsa = new RSACryptoServiceProvider())
                { rsa.PersistKeyInCsp = false; rsa.ImportParameters(new RSAParameters { Modulus = modulus, Exponent = exponent });
                  return rsa.VerifyData(transcript, CryptoConfig.MapNameToOID("SHA256"), Convert.FromBase64String(signature)); }
            }
            catch (Exception) { return false; }
        }
        public static string LanAddress()
        {
            foreach (var n in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
                if (n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up && n.NetworkInterfaceType != System.Net.NetworkInformation.NetworkInterfaceType.Loopback)
                    foreach (var a in n.GetIPProperties().UnicastAddresses)
                        if (a.Address.AddressFamily == AddressFamily.InterNetwork && !a.Address.ToString().StartsWith("169.254.")) return a.Address.ToString();
            return "127.0.0.1";
        }
    }

    public sealed class Invitation
    {
        public string Address, Fingerprint, Code, Name, Id;
        public int Port;
        public long Expires;
        public string Encode()
        { return "hlink1:" + Convert.ToBase64String(Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(this))).TrimEnd('=').Replace('+', '-').Replace('/', '_'); }
        public static Invitation Decode(string text)
        {
            if (text == null || text.Length > 4096 || !text.Trim().StartsWith("hlink1:")) throw new InvalidDataException("Paste a Hyperlink invitation from the host computer.");
            string v = text.Trim().Substring(7).Replace('-', '+').Replace('_', '/'); v = v.PadRight((v.Length + 3) / 4 * 4, '=');
            var invite = new JavaScriptSerializer().Deserialize<Invitation>(Encoding.UTF8.GetString(Convert.FromBase64String(v)));
            if (invite == null || String.IsNullOrWhiteSpace(invite.Address) || invite.Address.Length > 253 || invite.Port < 1 || invite.Port > 65535 ||
                invite.Fingerprint == null || invite.Fingerprint.Length != 64 || invite.Id == null || invite.Id.Length != 64 || invite.Code == null || invite.Code.Length > 128)
                throw new InvalidDataException("Invalid invitation.");
            if (DateTime.UtcNow.Ticks > invite.Expires) throw new InvalidDataException("This invitation expired. Create a fresh one on the host.");
            return invite;
        }
    }

    public sealed class Wire : IDisposable
    {
        public readonly TcpClient Client;
        public readonly SslStream Stream;
        readonly object writeLock = new object();
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 16384, RecursionLimit = 8 };
        public Wire(TcpClient client, SslStream stream) { Client = client; Stream = stream; Stream.WriteTimeout = 5000; }
        public void Send(byte kind, byte[] bytes)
        {
            if (bytes.Length > 4 * 1024 * 1024) throw new InvalidDataException("Packet too large.");
            var h = new byte[] { kind, (byte)(bytes.Length >> 24), (byte)(bytes.Length >> 16), (byte)(bytes.Length >> 8), (byte)bytes.Length };
            lock (writeLock) { Stream.Write(h, 0, h.Length); Stream.Write(bytes, 0, bytes.Length); Stream.Flush(); }
        }
        public void SendJson(object value) { Send(1, Encoding.UTF8.GetBytes(json.Serialize(value))); }
        public byte[] Read(out byte kind, int limit)
        {
            byte[] h = Exact(5); kind = h[0];
            int size = (h[1] << 24) | (h[2] << 16) | (h[3] << 8) | h[4];
            limit = Math.Min(limit, kind == 1 ? 16384 : kind == 11 ? 8208 : 4 * 1024 * 1024);
            if (size < 0 || size > limit) throw new InvalidDataException("Invalid packet size.");
            return Exact(size);
        }
        byte[] Exact(int n)
        { var b = new byte[n]; int p = 0; while (p < n) { int r = Stream.Read(b, p, n - p); if (r == 0) throw new EndOfStreamException(); p += r; } return b; }
        public Dictionary<string, object> ReadJson()
        {
            byte kind; var bytes = Read(out kind, 16384); if (kind != 1) throw new InvalidDataException("Expected control message.");
            var obj = json.DeserializeObject(Encoding.UTF8.GetString(bytes)) as Dictionary<string, object>;
            if (obj == null) throw new InvalidDataException("Invalid control message."); return obj;
        }
        public static string Text(Dictionary<string, object> obj, string key)
        { object v; if (!obj.TryGetValue(key, out v) || !(v is string)) throw new InvalidDataException("Missing field: " + key); return (string)v; }
        public static int Number(Dictionary<string, object> obj, string key)
        { object v; if (!obj.TryGetValue(key, out v) || !(v is int)) throw new InvalidDataException("Missing number: " + key); return (int)v; }
        public static Wire Connect(string address, int port, string fingerprint)
        {
            var client = new TcpClient(); client.NoDelay = true;
            try
            {
                var pending = client.BeginConnect(address, port, null, null);
                using (var wait = pending.AsyncWaitHandle) if (!wait.WaitOne(7000)) throw new TimeoutException("Host did not answer. Check its address, hosting state, and private-network firewall rule.");
                client.EndConnect(pending);
                var stream = new SslStream(client.GetStream(), false, delegate(object sender, X509Certificate cert, X509Chain chain, SslPolicyErrors errors)
                {
                    if (cert == null || !Util.Equal(Util.Hash(cert.GetRawCertData()), fingerprint)) return false;
                    using (var c = new X509Certificate2(cert)) return DateTime.UtcNow >= c.NotBefore.ToUniversalTime() && DateTime.UtcNow <= c.NotAfter.ToUniversalTime();
                });
                stream.ReadTimeout = 15000; stream.WriteTimeout = 10000;
                stream.AuthenticateAsClient("Hyperlink", null, SslProtocols.Tls12, false);
                return new Wire(client, stream);
            }
            catch { client.Close(); throw; }
        }
        public void Dispose() { try { Client.Close(); } catch { } try { Stream.Dispose(); } catch { } }
    }

    public sealed class Capture
    {
        public int Width, Height;
        public readonly bool Synthetic;
        readonly Rectangle bounds;
        public Capture(int monitor, bool synthetic)
        {
            Synthetic = synthetic;
            bounds = synthetic ? new Rectangle(0, 0, 160, 100) : Screen.AllScreens[Math.Max(0, Math.Min(Screen.AllScreens.Length - 1, monitor))].Bounds;
            double scale = Math.Min(1, Math.Min(1600.0 / bounds.Width, 1000.0 / bounds.Height));
            Width = Math.Max(1, (int)(bounds.Width * scale)); Height = Math.Max(1, (int)(bounds.Height * scale));
        }
        public byte[] Frame()
        {
            using (var raw = new Bitmap(bounds.Width, bounds.Height, PixelFormat.Format24bppRgb))
            {
                using (var g = Graphics.FromImage(raw))
                {
                    if (Synthetic) { g.Clear(Color.FromArgb(20, 35, 50)); g.FillRectangle(Brushes.Cyan, 20, 20, 60, 30); }
                    else
                    {
                        if (!InputController.DefaultDesktop()) throw new InvalidOperationException("The desktop is locked or protected. Unlock it locally to resume.");
                        g.CopyFromScreen(bounds.Location, Point.Empty, bounds.Size, CopyPixelOperation.SourceCopy);
                        var cursor = new InputController.CURSORINFO(); cursor.cbSize = Marshal.SizeOf(cursor);
                        if (InputController.GetCursorInfo(ref cursor) && cursor.flags == 1)
                        {
                            IntPtr dc = g.GetHdc();
                            try { InputController.DrawIconEx(dc, cursor.ptScreenPos.X - bounds.X, cursor.ptScreenPos.Y - bounds.Y, cursor.hCursor, 0, 0, 0, IntPtr.Zero, 3); }
                            finally { g.ReleaseHdc(dc); }
                        }
                    }
                }
                using (var resized = new Bitmap(raw, Width, Height))
                using (var buffer = new MemoryStream())
                using (var quality = new EncoderParameters(1))
                {
                    quality.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 72L);
                    resized.Save(buffer, ImageCodecInfo.GetImageEncoders().First(c => c.MimeType == "image/jpeg"), quality);
                    return buffer.ToArray();
                }
            }
        }
        public Point PointFor(int x, int y) { return new Point(bounds.X + x * bounds.Width / Width, bounds.Y + y * bounds.Height / Height); }
    }

    public sealed class InputController
    {
        readonly HashSet<int> keys = new HashSet<int>();
        readonly HashSet<int> buttons = new HashSet<int>();
        readonly bool synthetic;
        public int Applied;
        public InputController(bool simulate) { synthetic = simulate; }
        public void Apply(Dictionary<string, object> message, Capture capture)
        {
            string type = Wire.Text(message, "type");
            lock (keys)
            {
                if (!synthetic && !DefaultDesktop()) { Release(); return; }
                if (type == "move")
                {
                    int x = Wire.Number(message, "x"), y = Wire.Number(message, "y");
                    if (x < 0 || y < 0 || x >= capture.Width || y >= capture.Height) throw new InvalidDataException("Pointer outside display.");
                    Point p = capture.PointFor(x, y); if (!synthetic) Move(p);
                }
                else if (type == "text")
                {
                    string text = Wire.Text(message, "text");
                    if (text.Length < 1 || text.Length > 1024 || text.IndexOf('\0') >= 0) throw new InvalidDataException("Invalid text input.");
                    for (int i = 0; i < text.Length; i++)
                    {
                        if (Char.IsHighSurrogate(text[i]))
                        {
                            if (i + 1 >= text.Length || !Char.IsLowSurrogate(text[++i])) throw new InvalidDataException("Invalid Unicode text.");
                        }
                        else if (Char.IsLowSurrogate(text[i])) throw new InvalidDataException("Invalid Unicode text.");
                    }
                    if (!synthetic) SendText(text);
                }
                else if (type == "key")
                {
                    int key = Wire.Number(message, "key"), down = Wire.Number(message, "down");
                    if (key < 8 || key > 254 || (down != 0 && down != 1)) throw new InvalidDataException("Invalid key.");
                    if (down == 1) { if (!keys.Add(key)) return; } else { if (!keys.Remove(key)) return; }
                    if (!synthetic) SendKey(key, down == 1);
                }
                else if (type == "button")
                {
                    int button = Wire.Number(message, "button"), down = Wire.Number(message, "down");
                    if (button < 0 || button > 2 || (down != 0 && down != 1)) throw new InvalidDataException("Invalid button.");
                    if (down == 1) { if (!buttons.Add(button)) return; } else { if (!buttons.Remove(button)) return; }
                    if (!synthetic) SendMouse(button == 0 ? (down == 1 ? 2U : 4U) : button == 1 ? (down == 1 ? 8U : 16U) : (down == 1 ? 32U : 64U), 0);
                }
                else if (type == "wheel")
                { int delta = Wire.Number(message, "delta"); if (delta < -1200 || delta > 1200) throw new InvalidDataException("Invalid scroll."); if (!synthetic) SendMouse(0x0800, unchecked((uint)delta)); }
                else if (type == "release") Release();
                else throw new InvalidDataException("Unknown input event.");
                Applied++;
            }
        }
        public int Held { get { lock (keys) return keys.Count + buttons.Count; } }
        public void Release()
        {
            lock (keys)
            {
                foreach (int k in keys) if (!synthetic) SendKey(k, false);
                foreach (int b in buttons) if (!synthetic) SendMouse(b == 0 ? 4U : b == 1 ? 16U : 64U, 0);
                keys.Clear(); buttons.Clear();
            }
        }
        static void Move(Point p)
        {
            var desktop = SystemInformation.VirtualScreen;
            SendMouse(0x8000 | 0x4000 | 1, 0, (p.X - desktop.Left) * 65535 / Math.Max(1, desktop.Width - 1), (p.Y - desktop.Top) * 65535 / Math.Max(1, desktop.Height - 1));
        }
        static void SendText(string text)
        {
            foreach (char value in text)
            {
                var down = new INPUT { type = 1 };
                down.union.keyboard.wScan = (ushort)value; down.union.keyboard.dwFlags = 4;
                var up = down; up.union.keyboard.dwFlags = 6;
                if (SendInput(2, new[] { down, up }, Marshal.SizeOf(typeof(INPUT))) != 2)
                {
                    SendInput(1, new[] { up }, Marshal.SizeOf(typeof(INPUT)));
                    throw new InvalidOperationException("Windows did not accept text input.");
                }
            }
        }
        static void SendKey(int key, bool down)
        {
            var input = new INPUT { type = 1 }; input.union.keyboard.wVk = (ushort)key;
            input.union.keyboard.dwFlags = (down ? 0U : 2U) | ((key == 0xA3 || key == 0xA5 || (key >= 0x21 && key <= 0x28) || key == 0x2D || key == 0x2E || key == 0x5B || key == 0x5C) ? 1U : 0U);
            SendInput(1, new INPUT[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }
        static void SendMouse(uint flags, uint data, int x = 0, int y = 0)
        {
            var input = new INPUT { type = 0 }; input.union.mouse.dx = x; input.union.mouse.dy = y; input.union.mouse.mouseData = data; input.union.mouse.dwFlags = flags;
            SendInput(1, new INPUT[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }
        public static bool DefaultDesktop()
        {
            IntPtr d = OpenInputDesktop(0, false, 1); if (d == IntPtr.Zero) return false;
            try { var name = new StringBuilder(256); int required; return GetUserObjectInformation(d, 2, name, 512, out required) && name.ToString() == "Default"; }
            finally { CloseDesktop(d); }
        }
        [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION union; }
        [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mouse; [FieldOffset(0)] public KEYBDINPUT keyboard; }
        [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public UIntPtr dwExtraInfo; }
        [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public UIntPtr dwExtraInfo; }
        [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize, flags; public IntPtr hCursor; public Point ptScreenPos; }
        [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
        [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
        [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder info, int size, out int needed);
        [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO info);
        [DllImport("user32.dll")] public static extern bool DrawIconEx(IntPtr dc, int x, int y, IntPtr icon, int w, int h, uint step, IntPtr brush, uint flags);
    }

    public sealed class Host : IDisposable
    {
        public bool SessionActive { get { lock (gate) return active != null; } }
        public bool BeginAutomaticUpdate(Action start) { lock (gate) { if (active != null) return false; start(); Stop(); return true; } }
        readonly Store store;
        readonly Func<string, bool, int> approve;
        readonly Action<string> status;
        readonly object gate = new object();
        readonly bool synthetic;
        readonly HashSet<Wire> connections = new HashSet<Wire>();
        public readonly InputController Input;
        TcpListener listener;
        Wire active;
        string activePeer, inviteCode;
        long inviteExpires;
        int workers;
        public int Port { get; private set; }
        public int Monitor;
        internal Exception LastFailure;
        public bool Running { get { lock (gate) return listener != null; } }
        public bool Active { get { lock (gate) return active != null; } }
        public Host(Store state, Func<string, bool, int> confirmation, Action<string> changed, bool simulate = false)
        { store = state; approve = confirmation; status = changed; synthetic = simulate; Input = new InputController(simulate); }
        public void Start(IPAddress bind, int port)
        {
            lock (store.Sync) if (!String.IsNullOrEmpty(store.Data.FamilyDeviceId)) throw new InvalidOperationException("This computer uses family authorization. Hosted connections are still being integrated; direct pairing is disabled.");
            lock (gate)
            {
                if (listener != null) return;
                if (DateTime.UtcNow > store.Certificate.NotAfter.ToUniversalTime()) throw new InvalidOperationException("The host certificate expired. Hosting is disabled; re-enrollment is required.");
                var server = new TcpListener(bind, port); server.Start(8); listener = server; Port = ((IPEndPoint)server.LocalEndpoint).Port;
                new Thread(delegate() { Listen(server); }) { IsBackground = true, Name = "Hyperlink listener" }.Start();
            }
            status("Hosting is on. Connections require pairing and your approval.");
        }
        public Invitation Invite(string address)
        {
            if (!Running) throw new InvalidOperationException("Start hosting before creating an invitation.");
            if (String.IsNullOrWhiteSpace(address) || Uri.CheckHostName(address.Trim()) == UriHostNameType.Unknown) throw new InvalidDataException("Enter a valid host address.");
            lock (gate)
            {
                inviteCode = Util.Random(24); inviteExpires = DateTime.UtcNow.AddMinutes(5).Ticks;
                return new Invitation { Address = address.Trim(), Port = Port, Name = store.Data.Name, Fingerprint = store.Fingerprint, Id = store.Id, Code = inviteCode, Expires = inviteExpires };
            }
        }
        void Listen(TcpListener server)
        {
            while (true)
            {
                TcpClient client;
                try { client = server.AcceptTcpClient(); } catch { return; }
                lock (gate)
                {
                    if (listener != server) { client.Close(); return; }
                    if (workers >= 4) { client.Close(); continue; } workers++;
                }
                new Thread(delegate() { Handle(client, server); }) { IsBackground = true }.Start();
            }
        }
        void Handle(TcpClient client, TcpListener server)
        {
            Wire wire = null; bool ownsSession = false; SessionExtensions extensions = null;
            try
            {
                client.NoDelay = true;
                var ssl = new SslStream(client.GetStream(), false); ssl.ReadTimeout = 15000; ssl.WriteTimeout = 10000;
                wire = new Wire(client, ssl);
                lock (gate) { if (listener != server) return; connections.Add(wire); }
                ssl.AuthenticateAsServer(store.Certificate, false, SslProtocols.Tls12, false);
                var hello = wire.ReadJson(); if (Wire.Number(hello, "version") != 1) throw new InvalidDataException("Unsupported protocol version.");
                string operation = Wire.Text(hello, "operation");
                string id = Wire.Text(hello, "id"), publicKey = null, name = null, code = null;
                Peer peer = null;
                if (operation == "pair")
                {
                    code = Wire.Text(hello, "code");
                    lock (gate) if (!Util.Equal(code, inviteCode) || DateTime.UtcNow.Ticks > inviteExpires) throw new UnauthorizedAccessException();
                    publicKey = Wire.Text(hello, "publicKey"); name = Util.Name(Wire.Text(hello, "name"));
                    if (!Util.Equal(Util.Hash(publicKey), id)) throw new UnauthorizedAccessException();
                }
                else if (operation == "connect")
                {
                    lock (store.Sync) peer = store.Data.Peers.FirstOrDefault(p => p.Id == id);
                    if (peer == null) throw new UnauthorizedAccessException();
                    publicKey = peer.PublicKey; name = peer.Name;
                }
                else throw new UnauthorizedAccessException();
                string nonce = Util.Random(32);
                wire.SendJson(new { kind = "challenge", nonce = nonce });
                string sig = Wire.Text(wire.ReadJson(), "signature");
                if (!Util.Verify(publicKey, Util.Transcript(nonce, store.Fingerprint, id), sig)) throw new UnauthorizedAccessException();
                wire.Stream.ReadTimeout = 120000;
                if (operation == "pair")
                {
                    int permission = approve(name, true); if (permission < 1) throw new UnauthorizedAccessException();
                    lock (gate)
                    {
                        if (listener != server || !Util.Equal(code, inviteCode) || DateTime.UtcNow.Ticks > inviteExpires) throw new UnauthorizedAccessException();
                        lock (store.Sync)
                        {
                            if (store.Data.Peers.Count >= 32 && !store.Data.Peers.Any(p => p.Id == id)) throw new InvalidOperationException("Device limit reached.");
                            store.Data.Peers.RemoveAll(p => p.Id == id);
                            store.Data.Peers.Add(new Peer { Id = id, PublicKey = publicKey, Name = name, Control = permission == 2 }); store.Save();
                        }
                        inviteCode = null; inviteExpires = 0;
                    }
                    wire.SendJson(new { kind = "paired", control = permission == 2 });
                    status("Paired with " + name + ". Review or revoke it under Access."); return;
                }
                lock (gate) if (active != null) throw new InvalidOperationException("This computer already has an active viewer.");
                int approved = approve(name, false); if (approved < 1) throw new UnauthorizedAccessException();
                bool control;
                lock (gate)
                {
                    if (listener != server || active != null) throw new UnauthorizedAccessException();
                    lock (store.Sync) { peer = store.Data.Peers.FirstOrDefault(p => p.Id == id); if (peer == null) throw new UnauthorizedAccessException(); control = peer.Control && approved == 2; }
                    active = wire; activePeer = id; ownsSession = true;
                }
                var capture = new Capture(Monitor, synthetic);
                wire.SendJson(new { kind = "accepted", control = control, textInput = true, extensions = true, width = capture.Width, height = capture.Height, requestedFps = 30, name = store.Data.Name });
                status(name + " is connected · " + (control ? "View and control" : "View only"));
                wire.Stream.ReadTimeout = 10000;
                var sessionWire = wire;
                var sender = new Thread(delegate()
                {
                    try
                    {
                        while (true)
                        {
                            lock (gate) if (active != sessionWire || listener != server) break;
                            var timer = Stopwatch.StartNew();
                            byte[] frame = capture.Frame();
                            lock (gate) if (active != sessionWire || listener != server) break;
                            sessionWire.Send(10, frame);
                            int delay = 33 - (int)timer.ElapsedMilliseconds; if (delay > 0) Thread.Sleep(delay);
                        }
                    }
                    catch (Exception) { sessionWire.Dispose(); }
                }) { IsBackground = true, Name = "Hyperlink capture" }; sender.Start();
                while (true)
                {
                    var message = wire.ReadJson(); string kind = Wire.Text(message, "kind");
                    if (kind == "ping") continue;
                    if (kind == "extension")
                    {
                        string request = Wire.Text(message, "request"), extensionOperation = Wire.Text(message, "operation"); Guid requestId;
                        if (!Guid.TryParseExact(request, "N", out requestId)) throw new InvalidDataException("Invalid extension request.");
                        lock (gate)
                        {
                            if (active != wire || listener != server) break;
                            bool allowed; lock (store.Sync) allowed = SessionExtensions.Allowed(store.Data.Peers.FirstOrDefault(p => p.Id == id), extensionOperation);
                            object result = null; bool ok = false;
                            if (!allowed && extensions != null) { try { extensions.Dispose(); } catch { } extensions = null; }
                            if (allowed) try
                            {
                                if (extensions == null) extensions = new SessionExtensions(delegate { lock (store.Sync) return store.Data.SharedFolder; }, synthetic, delegate(byte[] packet)
                                {
                                    lock (gate)
                                    {
                                        if (active != wire || listener != server || extensions == null || !extensions.OwnsAudio(packet)) return false;
                                        lock (store.Sync) { var audioPeer = store.Data.Peers.FirstOrDefault(p => p.Id == id); if (audioPeer == null || !audioPeer.Audio) return false; }
                                        wire.Send(11, packet); return true;
                                    }
                                });
                                result = extensions.Handle(message); ok = true;
                            }
                            catch (Exception) { if (extensions != null) try { extensions.Dispose(); } catch { } extensions = null; }
                            wire.SendJson(new { kind = "extension", request = request, ok = ok, result = result });
                            if (ok && extensionOperation == "audio-start") extensions.StartAudio();
                        }
                        continue;
                    }
                    if (kind != "input") throw new InvalidDataException("Unknown session message.");
                    lock (gate)
                    {
                        if (active != wire || listener != server) break;
                        if (!control) throw new UnauthorizedAccessException();
                        lock (store.Sync) if (!store.Data.Peers.Any(p => p.Id == id && p.Control)) throw new UnauthorizedAccessException();
                        Input.Apply(message, capture);
                    }
                }
            }
            catch (Exception ex)
            {
                LastFailure = ex;
                if (wire != null) try { wire.SendJson(new { kind = "error", message = ex is UnauthorizedAccessException ? "Not authorized. Check pairing, local approval, and access grants." : ex is InvalidOperationException ? ex.Message : "Connection closed. Check the host and its authorization." }); } catch { }
            }
            finally
            {
                if (extensions != null) try { extensions.Dispose(); } catch { }
                if (wire != null) wire.Dispose(); else client.Close();
                lock (gate)
                {
                    if (wire != null) connections.Remove(wire);
                    if (ownsSession && active == wire) { active = null; activePeer = null; Input.Release(); }
                    workers--;
                }
                if (ownsSession) status("Session ended. All held keys and buttons were released.");
            }
        }
        public void Revoke(string id)
        {
            lock (gate)
            {
                lock (store.Sync) { store.Data.Peers.RemoveAll(p => p.Id == id); store.Save(); }
                if (activePeer == id) StopSession();
            }
            status("Access revoked. New and active connections are blocked.");
        }
        public void StopSession()
        {
            lock (gate)
            { var w = active; active = null; activePeer = null; if (w != null) w.Dispose(); Input.Release(); }
            status("Session stopped locally.");
        }
        public void Stop()
        {
            lock (gate)
            {
                var server = listener; listener = null; inviteCode = null; inviteExpires = 0;
                if (server != null) server.Stop();
                foreach (var c in connections.ToArray()) c.Dispose();
                active = null; activePeer = null; Input.Release();
            }
            status("Hosting is off. This computer is not accepting connections.");
        }
        public void Dispose() { Stop(); }
    }

    public sealed partial class Remote : IDisposable
    {
        readonly Store store;
        Wire wire;
        Thread reader;
        System.Threading.Timer heartbeat;
        int closed;
        public bool Control;
        public int Width, Height;
        public bool Connected { get { return wire != null; } }
        public Action<Bitmap> Frame;
        public Action<string> Ended;
        public Remote(Store state) { store = state; }
        void Authenticate(Wire connection, object hello, string fingerprint)
        {
            connection.SendJson(hello); var challenge = connection.ReadJson();
            if (Wire.Text(challenge, "kind") != "challenge") throw new UnauthorizedAccessException("The host rejected this identity or invitation.");
            string nonce = Wire.Text(challenge, "nonce");
            if (nonce.Length < 32 || nonce.Length > 128) throw new InvalidDataException("Invalid host challenge.");
            connection.SendJson(new { signature = store.Sign(Util.Transcript(nonce, fingerprint, store.Id)) });
            connection.Stream.ReadTimeout = 120000;
        }
        public Device Pair(Invitation invite)
        {
            using (var connection = Wire.Connect(invite.Address, invite.Port, invite.Fingerprint))
            {
                Authenticate(connection, new { version = 1, operation = "pair", id = store.Id, publicKey = store.PublicKey, name = store.Data.Name, code = invite.Code }, invite.Fingerprint);
                var reply = connection.ReadJson(); if (Wire.Text(reply, "kind") != "paired") throw new UnauthorizedAccessException("Pairing was declined or expired on the host.");
                var device = new Device { Id = invite.Id, Name = Util.Name(invite.Name), Address = invite.Address, Port = invite.Port, Fingerprint = invite.Fingerprint, Control = (bool)reply["control"] };
                lock (store.Sync) { store.Data.Devices.RemoveAll(d => d.Id == device.Id); store.Data.Devices.Add(device); store.Save(); }
                return device;
            }
        }
        public void Connect(Device device)
        {
            Wire connection = Wire.Connect(device.Address, device.Port, device.Fingerprint);
            try
            {
                if (Interlocked.CompareExchange(ref wire, connection, null) != null) throw new InvalidOperationException("Already connected.");
                if (Volatile.Read(ref closed) != 0) { Dispose(); throw new OperationCanceledException(); }
                Authenticate(connection, new { version = 1, operation = "connect", id = store.Id }, device.Fingerprint);
                var reply = connection.ReadJson();
                if (Wire.Text(reply, "kind") != "accepted") throw new UnauthorizedAccessException(Wire.Text(reply, "message"));
                Width = Wire.Number(reply, "width"); Height = Wire.Number(reply, "height");
                object supports; SupportsExtensions = reply.TryGetValue("extensions", out supports) && supports is bool && (bool)supports;
                if (Width < 1 || Width > 1600 || Height < 1 || Height > 1000) throw new InvalidDataException("Invalid frame dimensions.");
                Control = (bool)reply["control"]; connection.Stream.ReadTimeout = 15000;
                if (Volatile.Read(ref closed) != 0) throw new OperationCanceledException();
                heartbeat = new System.Threading.Timer(delegate { try { connection.SendJson(new { kind = "ping" }); } catch { connection.Dispose(); } }, null, 2000, 2000);
                reader = new Thread(delegate()
                {
                    string message = "Session disconnected.";
                    try
                    {
                        while (true)
                        {
                            byte kind; var bytes = connection.Read(out kind, 4 * 1024 * 1024);
                            if (kind == 1) { AcceptExtension(bytes); continue; }
                            if (kind == 11) { AcceptAudio(bytes); continue; }
                            if (kind != 10) throw new InvalidDataException("Unexpected frame.");
                            using (var stream = new MemoryStream(bytes)) using (var decoded = Image.FromStream(stream, true, true))
                            {
                                if (decoded.Width != Width || decoded.Height != Height) throw new InvalidDataException("Frame size changed unexpectedly.");
                                var bitmap = new Bitmap(decoded); var callback = Frame;
                                if (callback != null) callback(bitmap); else bitmap.Dispose();
                            }
                        }
                    }
                    catch (Exception ex) { if (ex is InvalidDataException) message = ex.Message; }
                    finally { Dispose(); var callback = Ended; if (callback != null) callback(message); }
                }) { IsBackground = true, Name = "Hyperlink viewer" }; reader.Start();
            }
            catch { Dispose(); connection.Dispose(); throw; }
        }
        public void Send(object message)
        { var w = wire; if (w != null && Control) try { w.SendJson(message); } catch { w.Dispose(); } }
        public void Dispose()
        {
            Interlocked.Exchange(ref closed, 1);
            var timer = Interlocked.Exchange(ref heartbeat, null); if (timer != null) timer.Dispose();
            var connection = Interlocked.Exchange(ref wire, null); if (connection != null) connection.Dispose();
        }
    }
}
