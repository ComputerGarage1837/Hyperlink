using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace Hyperlink
{
    static class UpdateTests
    {
        static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
        static void Denied(Action action) { try { action(); } catch (InvalidDataException) { return; } throw new Exception("Unsafe update accepted."); }
        static byte[] Sign(ReleaseManifest manifest, RSACryptoServiceProvider rsa)
        {
            var json = new JavaScriptSerializer(); byte[] payload = Encoding.UTF8.GetBytes(json.Serialize(manifest));
            return Encoding.UTF8.GetBytes(json.Serialize(new ReleaseEnvelope { payload = Convert.ToBase64String(payload), signature = Convert.ToBase64String(rsa.SignData(payload, CryptoConfig.MapNameToOID("SHA256"))) }));
        }
        internal static string Run(string root)
        {
            Directory.CreateDirectory(root); string install = Path.Combine(root, "install"), stage = Path.Combine(root, "stage"), backup = Path.Combine(root, "backup");
            Directory.CreateDirectory(install); Directory.CreateDirectory(stage); Directory.CreateDirectory(Path.Combine(install, "Data"));
            string identity = Path.Combine(install, "Data", "identity.dat"); File.WriteAllText(identity, "preserve-owner-identity");
            foreach (string name in ReleasePackages.Names) { File.WriteAllText(Path.Combine(install, name), "original-" + name); File.WriteAllText(Path.Combine(stage, name), "updated-" + name); }
            var manifest = new ReleaseManifest { product = "Hyperlink", version = "0.5.0.0", sequence = 5,
                files = ReleasePackages.Names.Select(name => new ReleaseFile { name = name, size = new FileInfo(Path.Combine(stage, name)).Length, sha256 = ReleasePackages.Hash(Path.Combine(stage, name)) }).ToArray() };
            using (var key = new RSACryptoServiceProvider(3072))
            {
                key.PersistKeyInCsp = false; string publicKey = key.ToXmlString(false); byte[] signed = Sign(manifest, key);
                Check(ReleasePackages.Verify(signed, 4, publicKey).sequence == 5, "Signed manifest rejected.");
                Denied(() => ReleasePackages.Verify(signed, 5, publicKey));
                Denied(() => ReleasePackages.Verify(signed, 6, publicKey));
                var envelope = new JavaScriptSerializer().Deserialize<ReleaseEnvelope>(Encoding.UTF8.GetString(signed));
                byte[] payload = Convert.FromBase64String(envelope.payload); payload[1] ^= 1; envelope.payload = Convert.ToBase64String(payload);
                Denied(() => ReleasePackages.Verify(Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(envelope)), 0, publicKey));
                using (var wrong = new RSACryptoServiceProvider(3072)) { wrong.PersistKeyInCsp = false; Denied(() => ReleasePackages.Verify(signed, 0, wrong.ToXmlString(false))); }
                string first = manifest.files[0].name; manifest.files[0].name = "../Hyperlink.exe"; Denied(() => ReleasePackages.Verify(Sign(manifest, key), 0, publicKey)); manifest.files[0].name = first;
                string second = manifest.files[1].name; manifest.files[1].name = first; Denied(() => ReleasePackages.Verify(Sign(manifest, key), 0, publicKey)); manifest.files[1].name = second;
                long size = manifest.files[0].size; manifest.files[0].size = 193 * 1024 * 1024; Denied(() => ReleasePackages.Verify(Sign(manifest, key), 0, publicKey)); manifest.files[0].size = size;
            }
            var original = UpdateTransaction.Backup(install, backup);
            bool interrupted = false;
            try { UpdateTransaction.Apply(install, stage, manifest, index => { if (index == 2) throw new IOException("Simulated interruption"); }); } catch (IOException) { interrupted = true; }
            Check(interrupted && ReleasePackages.Hash(Path.Combine(install, manifest.files[0].name)) == manifest.files[0].sha256, "Interruption fixture did not replace a file.");
            UpdateTransaction.Restore(install, backup, original);
            Check(original.All(p => ReleasePackages.Hash(Path.Combine(install, p.Key)) == p.Value), "Interrupted update did not restore all old files.");
            Check(File.ReadAllText(identity) == "preserve-owner-identity", "Update recovery replaced identity data.");
            UpdateTransaction.Apply(install, stage, manifest, null); ReleasePackages.VerifyFiles(install, manifest);
            Check(File.ReadAllText(identity) == "preserve-owner-identity", "Successful update replaced identity data.");
            File.WriteAllText(Path.Combine(stage, "README.md"), "tampered"); Denied(() => UpdateTransaction.Apply(install, stage, manifest, null));
            Check(ReleasePackages.Hash(Path.Combine(install, "README.md")) == manifest.files.First(f => f.name == "README.md").sha256, "Tampered update changed installed files.");
            File.WriteAllText(Path.Combine(backup, "README.md"), "corrupt backup"); bool refused = false;
            try { UpdateTransaction.Restore(install, backup, original); } catch (IOException) { refused = true; }
            Check(refused && ReleasePackages.Hash(Path.Combine(install, "Hyperlink.exe")) == manifest.files[0].sha256, "Corrupt backup caused partial restoration.");
            return "Signed-update and recovery checks passed, including tampering, downgrade refusal, interrupted replacement and identity preservation.";
        }
    }
}
