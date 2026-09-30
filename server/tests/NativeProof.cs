using System;
using System.IO;
using System.Security.Cryptography;
using System.Web.Script.Serialization;

namespace Hyperlink
{
    static class NativeProof
    {
        static int Main(string[] args)
        {
            using (var key = new RSACryptoServiceProvider(2048)) {
                key.PersistKeyInCsp = false;
                var client = new AccountClient(key, "https://hyperlink.test");
                var result = new { proof = client.Proof("POST", "/v1/devices", new String('a', 43)), jkt = client.Thumbprint };
                File.WriteAllText(args[0], new JavaScriptSerializer().Serialize(result));
            }
            return 0;
        }
    }
}
