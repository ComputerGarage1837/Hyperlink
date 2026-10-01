using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace Hyperlink
{
    sealed class RelayProvisioning
    {
        public string Id { get; set; } public string Fingerprint { get; set; } public string Code { get; set; } public string Token { get; set; } public string Enrollment { get; set; } public string Attestation { get; set; }
        internal static RelayProvisioning Load(Store store)
        {
            string file = Path.Combine(store.DataDirectory, "relay-provisioning.dpapi");
            if (!File.Exists(file)) throw new IOException("This Windows installation has not been enrolled on your relay.");
            byte[] clear = ProtectedData.Unprotect(File.ReadAllBytes(file), null, DataProtectionScope.CurrentUser);
            try
            {
                var value = new JavaScriptSerializer { MaxJsonLength = 4096, RecursionLimit = 8 }.Deserialize<RelayProvisioning>(new UTF8Encoding(false, true).GetString(clear));
                if (value == null || value.Id != store.Id || value.Fingerprint != store.Fingerprint || !HostAuthority.Verify(value.Id, value.Fingerprint, value.Code, value.Attestation) || !System.Text.RegularExpressions.Regex.IsMatch(value.Token ?? "", "^[a-f0-9]{64}$")) throw new IOException("Relay enrollment does not match this computer.");
                return value;
            }
            finally { Array.Clear(clear, 0, clear.Length); }
        }
    }
}
