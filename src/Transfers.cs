using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace Hyperlink
{
    // Paths are chosen by the owner. A remote peer can address only plain files in that folder.
    sealed class FileTransfers : IDisposable
    {
        const long Maximum = 1024L * 1024 * 1024;
        readonly string root;
        FileStream upload, download;
        SHA256 uploadHash, downloadHash;
        string temporary, destination, uploadId, downloadId;
        long expected, written;
        public FileTransfers(string folder)
        {
            if (String.IsNullOrWhiteSpace(folder)) throw new UnauthorizedAccessException("The owner has not selected a shared folder.");
            root = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            if (root.StartsWith("\\\\", StringComparison.Ordinal) || new DriveInfo(Path.GetPathRoot(root)).DriveType == DriveType.Network) throw new UnauthorizedAccessException("Choose a local shared folder; network shares are not supported by this draft.");
            CheckRoot();
        }
        void CheckRoot()
        {
            var directory = new DirectoryInfo(root);
            if (!directory.Exists) throw new DirectoryNotFoundException();
            for (; directory != null; directory = directory.Parent)
                if ((directory.Attributes & FileAttributes.ReparsePoint) != 0) throw new UnauthorizedAccessException("Shared folders cannot use junctions or symbolic links.");
        }
        string PathFor(string name)
        {
            if (String.IsNullOrEmpty(name) || name.Length > 180 || name != Path.GetFileName(name) || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || name.EndsWith(".") || name.EndsWith(" ") || name.StartsWith(".hyperlink-", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Choose a plain file name.");
            string stem = name.Split('.')[0].ToUpperInvariant();
            if (new[] { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" }.Contains(stem) || (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && "0123456789\u00b9\u00b2\u00b3".IndexOf(stem[3]) >= 0)) throw new InvalidDataException("Reserved file name.");
            CheckRoot();
            string path = Path.Combine(root, name);
            if ((File.Exists(path) || Directory.Exists(path)) && (File.GetAttributes(path) & (FileAttributes.ReparsePoint | FileAttributes.Directory)) != 0) throw new UnauthorizedAccessException();
            return path;
        }
        static string Hex(byte[] bytes) { return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant(); }
        static void HashBlock(HashAlgorithm hash, byte[] bytes) { hash.TransformBlock(bytes, 0, bytes.Length, null, 0); }
        public object List(int offset)
        {
            if (offset < 0 || offset > 100000) throw new InvalidDataException();
            CheckRoot();
            var files = Directory.EnumerateFiles(root).Where(p => !Path.GetFileName(p).StartsWith(".hyperlink-", StringComparison.OrdinalIgnoreCase))
                .Where(p => (File.GetAttributes(p) & FileAttributes.ReparsePoint) == 0).OrderBy(p => p, StringComparer.OrdinalIgnoreCase).Skip(offset).Take(9).ToArray();
            return new { files = files.Take(8).Select(p => new { name = Path.GetFileName(p), size = new FileInfo(p).Length }).ToArray(), next = files.Length > 8 ? offset + 8 : -1 };
        }
        public object BeginUpload(string name, long length, string sha)
        {
            if (upload != null || length < 0 || length > Maximum || sha == null || sha.Length != 64 || sha.Any(c => !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))) throw new InvalidDataException("Invalid upload.");
            destination = PathFor(name);
            if (File.Exists(destination)) throw new IOException("A file with this name already exists. Rename it first.");
            uploadId = Guid.NewGuid().ToString("N");
            temporary = Path.Combine(root, ".hyperlink-" + uploadId + ".part");
            upload = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            uploadHash = SHA256.Create(); expected = length; written = 0; expectedHash = sha;
            return new { id = uploadId, size = expected };
        }
        string expectedHash;
        public object Write(string id, long offset, string encoded)
        {
            if (upload == null || id != uploadId || offset != written || encoded == null || encoded.Length > 10924) throw new InvalidDataException("Invalid upload chunk.");
            byte[] bytes = Convert.FromBase64String(encoded);
            if (bytes.Length < 1 || bytes.Length > 8192 || written + bytes.Length > expected) throw new InvalidDataException("Upload exceeds its declared size.");
            CheckRoot(); upload.Write(bytes, 0, bytes.Length); HashBlock(uploadHash, bytes); written += bytes.Length;
            return new { offset = written };
        }
        public object Commit(string id)
        {
            if (upload == null || id != uploadId || written != expected) throw new InvalidDataException("Incomplete upload.");
            uploadHash.TransformFinalBlock(new byte[0], 0, 0);
            if (Hex(uploadHash.Hash) != expectedHash) { CancelUpload(); throw new InvalidDataException("File integrity check failed."); }
            CheckRoot(); upload.Flush(true); upload.Dispose(); upload = null;
            // Same-directory rename publishes only a complete, verified file. Existing files are never overwritten.
            try { File.Move(temporary, destination); }
            finally { if (File.Exists(temporary)) File.Delete(temporary); temporary = null; uploadHash.Dispose(); uploadHash = null; }
            return new { complete = true, sha256 = expectedHash, size = written };
        }
        public object BeginDownload(string name)
        {
            if (download != null) throw new InvalidDataException("A download is already active.");
            string path = PathFor(name); download = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            if (download.Length > Maximum) { download.Dispose(); download = null; throw new IOException("File exceeds the 1 GiB draft limit."); }
            downloadHash = SHA256.Create(); downloadId = Guid.NewGuid().ToString("N");
            return new { id = downloadId, size = download.Length, name = name };
        }
        public object Read(string id, long offset)
        {
            if (download == null || id != downloadId || offset != download.Position) throw new InvalidDataException("Invalid download position.");
            CheckRoot(); var buffer = new byte[8192]; int count = download.Read(buffer, 0, buffer.Length);
            if (count > 0) { if (count != buffer.Length) Array.Resize(ref buffer, count); HashBlock(downloadHash, buffer); }
            if (download.Position == download.Length)
            {
                downloadHash.TransformFinalBlock(new byte[0], 0, 0); string sha = Hex(downloadHash.Hash); long end = download.Position;
                download.Dispose(); download = null; downloadHash.Dispose(); downloadHash = null;
                return new { offset = end, data = Convert.ToBase64String(buffer, 0, count), complete = true, sha256 = sha };
            }
            return new { offset = download.Position, data = Convert.ToBase64String(buffer, 0, count), complete = false };
        }
        void CancelUpload()
        {
            if (upload != null) { upload.Dispose(); upload = null; }
            if (uploadHash != null) { uploadHash.Dispose(); uploadHash = null; }
            if (temporary != null) { if (File.Exists(temporary)) File.Delete(temporary); temporary = null; }
        }
        public void Dispose()
        {
            try { CancelUpload(); }
            finally { if (download != null) download.Dispose(); if (downloadHash != null) downloadHash.Dispose(); download = null; downloadHash = null; }
        }
    }
}
