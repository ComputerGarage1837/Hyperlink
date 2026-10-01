using System;
using System.IO;
using System.Net;
using System.Threading;

namespace Hyperlink
{
    static class InteropHost
    {
        static void Wait(string marker)
        {
            DateTime deadline = DateTime.UtcNow.AddSeconds(90);
            while (!File.Exists(marker)) { if (DateTime.UtcNow > deadline) throw new TimeoutException("Java fixture timed out"); Thread.Sleep(100); }
        }
        public static int Main(string[] args)
        {
            string root = Path.GetFullPath(args[0]);
            Directory.CreateDirectory(root);
            Host host = null;
            try
            {
                using (var store = new Store(Path.Combine(root, "identity")))
                {
                    host = new Host(store, delegate(string name, bool pair) { return name == "Java read-only test" ? 1 : 2; }, delegate { }, true);
                    host.Start(IPAddress.Loopback, 0);
                    File.WriteAllText(Path.Combine(root, "control.invite"), host.Invite("127.0.0.1").Encode());
                    Wait(Path.Combine(root, "control.paired"));
                    lock (store.Sync)
                    {
                        var peer = store.Data.Peers.Find(p => p.Name == "Java interoperability test");
                        if (peer == null) throw new Exception("Java pairing identity missing");
                        peer.FileRead = peer.FileWrite = peer.ClipboardToHost = peer.ClipboardFromHost = peer.Audio = true;
                        store.Data.SharedFolder = Path.Combine(root, "shared");
                        Directory.CreateDirectory(store.Data.SharedFolder);
                        store.Save();
                    }
                    File.WriteAllText(Path.Combine(root, "control.permissions"), "ready");
                    Wait(Path.Combine(root, "control.done"));
                    int before = host.Input.Applied;
                    if (before < 1) throw new Exception("Controlled input was not applied");
                    File.WriteAllText(Path.Combine(root, "readonly.invite"), host.Invite("127.0.0.1").Encode());
                    Wait(Path.Combine(root, "readonly.done"));
                    Thread.Sleep(300);
                    if (host.Input.Applied != before || host.Input.Held != 0) throw new Exception("Read-only client injected input");
                    File.WriteAllText(Path.Combine(root, "result.txt"), "Java TLS pairing, RSA challenge, JPEG, Unicode input and server-side read-only denial passed.");
                    host.Stop(); host = null;
                }
                return 0;
            }
            catch(Exception error) { File.WriteAllText(Path.Combine(root, "result.txt"), error.ToString()); return 1; }
            finally { if (host != null) host.Stop(); }
        }
    }
}
