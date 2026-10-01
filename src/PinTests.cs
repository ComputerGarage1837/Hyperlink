using System;
using System.Drawing;
using System.IO;
using System.Net;
using System.Threading;

namespace Hyperlink
{
    static class PinTests
    {
        internal static void Pair(Store viewer, Store owner, int port, string pin)
        {
            using (var wire = Wire.Connect("127.0.0.1", port, owner.Fingerprint))
            {
                wire.SendJson(new { version = 1, operation = "pair-pin", id = viewer.Id, publicKey = viewer.PublicKey, name = "Phone", pin = pin });
                var challenge = wire.ReadJson();
                if (Wire.Text(challenge, "kind") != "challenge") throw new UnauthorizedAccessException();
                wire.SendJson(new { signature = viewer.Sign(Util.Transcript(Wire.Text(challenge, "nonce"), owner.Fingerprint, viewer.Id)) });
                if (Wire.Text(wire.ReadJson(), "kind") != "paired") throw new UnauthorizedAccessException();
                wire.SendJson(new { kind = "paired-received" });
            }
        }
        static void Denied(Action action)
        {
            bool refused = false; try { action(); } catch { refused = true; }
            if (!refused) throw new Exception("Invalid unattended PIN authorization was accepted.");
        }
        internal static string Run(string folder)
        {
            Directory.CreateDirectory(folder);
            using (var owner = new Store(Path.Combine(folder, "owner"))) using (var phone = new Store(Path.Combine(folder, "phone")))
            using (var host = new Host(owner, delegate { throw new Exception("PIN access must not request local approval."); }, delegate { }, true))
            {
                host.Start(IPAddress.Loopback, 0);
                Denied(delegate { Pair(phone, owner, host.Port, "83275164"); });
                lock (owner.Sync) { PinGate.Configure(owner.Data, "83275164"); owner.Save(); }
                Denied(delegate { Pair(phone, owner, host.Port, "00000000"); });
                Pair(phone, owner, host.Port, "83275164");
                Peer granted = owner.Data.Peers.Find(p => p.Id == phone.Id);
                if (granted == null || !granted.Unattended || !granted.Control || granted.Audio || granted.Recording || granted.Privacy || granted.FileRead || granted.FileWrite)
                    throw new Exception("PIN pairing granted incorrect rights.");
                var target = new Device { Id = owner.Id, Fingerprint = owner.Fingerprint, Name = "Host", Address = "127.0.0.1", Port = host.Port };
                using (var remote = new Remote(phone)) using (var arrived = new ManualResetEvent(false))
                {
                    remote.Frame = delegate(Bitmap frame) { frame.Dispose(); arrived.Set(); };
                    remote.Connect(target); if (!arrived.WaitOne(5000)) throw new Exception("Unattended desktop frames did not arrive.");
                }
                host.Stop();
                var gate = new PinGate();
                for (int i = 0; i < 5; i++) if (gate.Check(owner.Data, "x")) throw new Exception("Invalid PIN accepted.");
                if (gate.Check(owner.Data, "83275164")) throw new Exception("PIN lockout was bypassed.");
                gate.Reset(); if (!gate.Check(owner.Data, "83275164")) throw new Exception("Owner lockout reset failed.");
                owner.Data.Unattended = false; if (gate.Check(owner.Data, "83275164")) throw new Exception("Disabled PIN access accepted.");
                if (owner.Data.PinHash.Contains("83275164")) throw new Exception("Plain PIN stored.");
            }
            return "PIN checks passed: disabled/wrong PIN refusal, identity proof, explicit unattended grants, no local prompts, independent tool rights and retry lockout.";
        }
    }
}
