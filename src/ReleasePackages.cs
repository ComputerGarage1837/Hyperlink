using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace Hyperlink
{
    sealed class ReleaseFile { public string name { get; set; } public string sha256 { get; set; } public long size { get; set; } }
    sealed class ReleaseManifest { public string product { get; set; } public string version { get; set; } public int sequence { get; set; } public ReleaseFile[] files { get; set; } }
    sealed class ReleaseEnvelope { public string payload { get; set; } public string signature { get; set; } }
    static class ReleasePackages
    {
        internal static readonly string[] Names = { "Hyperlink.exe", "NAudio.Core.dll", "NAudio.Wasapi.dll", "NAudio-MIT.txt", "README.md", "self-test.txt" };
        internal static readonly string[] OptionalNames = { "ffmpeg.exe", "ffmpeg-LGPL.txt", "ffmpeg-source.txt" };
        internal static readonly string[] AllowedNames = Names.Concat(OptionalNames).ToArray();
        internal static string Hash(string path)
        {
            using (var sha = SHA256.Create()) using (var file = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "").ToLowerInvariant();
        }
        internal static ReleaseManifest Verify(byte[] envelope, int floor, string publicKey)
        {
            if (envelope.Length < 1 || envelope.Length > 32768) throw new InvalidDataException("Invalid release envelope size.");
            var json = new JavaScriptSerializer { MaxJsonLength = 32768, RecursionLimit = 8 };
            var signed = json.Deserialize<ReleaseEnvelope>(new UTF8Encoding(false, true).GetString(envelope));
            if (signed == null || signed.payload == null || signed.signature == null) throw new InvalidDataException("Missing release signature.");
            byte[] payload = Convert.FromBase64String(signed.payload), signature = Convert.FromBase64String(signed.signature);
            if (payload.Length > 16384 || signature.Length != 384) throw new InvalidDataException("Invalid release signature size.");
            using (var rsa = new RSACryptoServiceProvider())
            {
                rsa.PersistKeyInCsp = false; rsa.FromXmlString(publicKey);
                if (!rsa.VerifyData(payload, CryptoConfig.MapNameToOID("SHA256"), signature)) throw new InvalidDataException("The release signature is invalid.");
            }
            var manifest = json.Deserialize<ReleaseManifest>(new UTF8Encoding(false, true).GetString(payload));
            Version version;
            if (manifest == null || manifest.product != "Hyperlink" || manifest.sequence <= floor || manifest.sequence < 1 ||
                !Version.TryParse(manifest.version, out version) || version.Build < 0 || version.Revision < 0 || manifest.files == null || manifest.files.Length < Names.Length || manifest.files.Length > AllowedNames.Length)
                throw new InvalidDataException("Invalid, reused, or older release.");
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var file in manifest.files)
            {
                if (file == null || !AllowedNames.Contains(file.name, StringComparer.Ordinal) || !seen.Add(file.name) || file.size < 1 || file.size > 192 * 1024 * 1024 ||
                    file.sha256 == null || !System.Text.RegularExpressions.Regex.IsMatch(file.sha256, "\\A[a-f0-9]{64}\\z"))
                    throw new InvalidDataException("Invalid release file inventory.");
            }
            if (!Names.All(seen.Contains) || (OptionalNames.Any(seen.Contains) && !OptionalNames.All(seen.Contains))) throw new InvalidDataException("Release is missing required runtime files or notices.");
            if (manifest.files.Sum(f => f.size) > 256 * 1024 * 1024) throw new InvalidDataException("Release is too large.");
            return manifest;
        }
        internal static ReleaseManifest Stage(string package, string destination, int floor)
        {
            if (new FileInfo(package).Length > 256 * 1024 * 1024) throw new InvalidDataException("Release package is too large.");
            Directory.CreateDirectory(destination);
            using (var archive = ZipFile.OpenRead(package))
            {
                if (archive.Entries.Count < Names.Length + 1 || archive.Entries.Count > AllowedNames.Length + 1) throw new InvalidDataException("Unexpected package entries.");
                var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.Ordinal);
                foreach (var entry in archive.Entries)
                {
                    if ((entry.FullName != "release.json" && !AllowedNames.Contains(entry.FullName, StringComparer.Ordinal)) || entries.ContainsKey(entry.FullName) ||
                        ((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000)
                        throw new InvalidDataException("Unsafe package entry.");
                    entries.Add(entry.FullName, entry);
                }
                var envelope = entries["release.json"];
                if (envelope.Length < 1 || envelope.Length > 32768) throw new InvalidDataException("Invalid release manifest size.");
                byte[] signed;
                using (var input = envelope.Open()) using (var output = new MemoryStream()) { CopyBounded(input, output, envelope.Length); signed = output.ToArray(); }
                var manifest = Verify(signed, floor, ReleaseKey.PublicXml);
                if (entries.Count != manifest.files.Length + 1) throw new InvalidDataException("Package contains unsigned files.");
                foreach (var file in manifest.files)
                {
                    var entry = entries[file.name];
                    if (entry.Length != file.size) throw new InvalidDataException("Release length mismatch.");
                    string path = Path.Combine(destination, file.name);
                    using (var input = entry.Open()) using (var output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    { CopyBounded(input, output, file.size); output.Flush(true); }
                    if (Hash(path) != file.sha256) throw new InvalidDataException("Release integrity check failed.");
                }
                File.WriteAllBytes(Path.Combine(destination, "release.json"), signed);
                var assembly = System.Reflection.AssemblyName.GetAssemblyName(Path.Combine(destination, "Hyperlink.exe"));
                if (assembly.Name != "Hyperlink" || assembly.Version.ToString() != manifest.version) throw new InvalidDataException("Release executable version mismatch.");
                return manifest;
            }
        }
        static void CopyBounded(Stream input, Stream output, long expected)
        {
            byte[] buffer = new byte[65536]; long total = 0; int count;
            while ((count = input.Read(buffer, 0, buffer.Length)) != 0)
            { total += count; if (total > expected) throw new InvalidDataException("Oversized release entry."); output.Write(buffer, 0, count); }
            if (total != expected) throw new InvalidDataException("Truncated release entry.");
        }
        internal static void VerifyFiles(string directory, ReleaseManifest manifest)
        {
            foreach (var file in manifest.files)
            {
                string path = Path.Combine(directory, file.name);
                if (!File.Exists(path) || new FileInfo(path).Length != file.size || Hash(path) != file.sha256)
                    throw new InvalidDataException("Release files changed after verification.");
            }
        }
    }
    static class UpdateTransaction
    {
        internal static Dictionary<string, string> Backup(string install, string backup)
        {
            Directory.CreateDirectory(backup); var hashes = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (string name in ReleasePackages.AllowedNames)
            {
                string source = Path.Combine(install, name);
                if (!File.Exists(source)) continue;
                string copy = Path.Combine(backup, name); CopyDurable(source, copy);
                string hash = ReleasePackages.Hash(source);
                if (ReleasePackages.Hash(copy) != hash) throw new IOException("Update backup integrity check failed.");
                hashes.Add(name, hash);
            }
            return hashes;
        }
        internal static void Apply(string install, string stage, ReleaseManifest manifest, Action<int> afterFile)
        {
            ReleasePackages.VerifyFiles(stage, manifest); int index = 0;
            foreach (var file in manifest.files)
            {
                Replace(Path.Combine(stage, file.name), Path.Combine(install, file.name));
                if (afterFile != null) afterFile(++index);
            }
            ReleasePackages.VerifyFiles(install, manifest);
        }
        internal static void Restore(string install, string backup, Dictionary<string, string> original)
        {
            // Check every backup before changing any installed file.
            foreach (var item in original)
                if (!ReleasePackages.AllowedNames.Contains(item.Key, StringComparer.Ordinal) || ReleasePackages.Hash(Path.Combine(backup, item.Key)) != item.Value)
                    throw new IOException("Update recovery backup is incomplete or corrupt.");
            foreach (string name in ReleasePackages.AllowedNames)
            {
                string target = Path.Combine(install, name);
                if (original.ContainsKey(name)) Replace(Path.Combine(backup, name), target);
                else if (File.Exists(target)) File.Delete(target);
            }
        }
        static void Replace(string source, string target)
        {
            string temporary = target + ".hyperlink-update.tmp";
            try { CopyDurable(source, temporary); if (File.Exists(target)) File.Replace(temporary, target, null); else File.Move(temporary, target); }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
        static void CopyDurable(string source, string target)
        {
            using (var input = File.OpenRead(source)) using (var output = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None))
            { input.CopyTo(output); output.Flush(true); }
        }
    }
}
