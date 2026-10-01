using System;
using System.Security.Cryptography;
using System.Text;

namespace Hyperlink
{
    static partial class HostAuthority
    {
        internal static bool Verify(string id, string fingerprint, string code, string attestation)
        {
            try
            {
                if (!System.Text.RegularExpressions.Regex.IsMatch(id ?? "", "^[a-f0-9]{64}$") || !System.Text.RegularExpressions.Regex.IsMatch(fingerprint ?? "", "^[a-f0-9]{64}$") || !System.Text.RegularExpressions.Regex.IsMatch(code ?? "", "^[1-9][0-9]{7}$")) return false;
                byte[] signature = Convert.FromBase64String(attestation ?? ""); if (signature.Length != 384) return false;
                using (var key = new RSACryptoServiceProvider()) { key.PersistKeyInCsp = false; key.FromXmlString(PublicKey); return key.VerifyData(Encoding.UTF8.GetBytes("Hyperlink host/1\n" + id + "\n" + fingerprint + "\n" + code), CryptoConfig.MapNameToOID("SHA256"), signature); }
            }
            catch (FormatException) { return false; } catch (CryptographicException) { return false; }
        }
    }
}
