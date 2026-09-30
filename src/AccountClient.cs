using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace Hyperlink
{
    // Family API traffic uses ordinary validated HTTPS plus RFC 9449 key-bound proofs.
    // The existing device identity supplies the signing key; no password or MFA secret is retained.
    sealed class AccountClient
    {
        readonly RSACryptoServiceProvider key;
        readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = 262144, RecursionLimit = 12 };
        readonly Uri origin;
        readonly object gate = new object();
        string accessToken;
        public string SessionId { get; private set; }
        public string Thumbprint { get; private set; }
        public bool SignedIn { get { lock (gate) return accessToken != null; } }

        public AccountClient(RSACryptoServiceProvider identity, string server)
        {
            key = identity;
            origin = new Uri(server.TrimEnd('/') + "/", UriKind.Absolute);
            if (origin.Scheme != Uri.UriSchemeHttps || origin.UserInfo.Length != 0 || origin.Query.Length != 0 ||
                origin.Fragment.Length != 0 || origin.AbsolutePath != "/") throw new ArgumentException("An HTTPS server origin is required.");
            Thumbprint = B64(Hash(Encoding.UTF8.GetBytes(CanonicalJwk())));
        }
        internal static string B64(byte[] bytes) { return Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_'); }
        static byte[] Hash(byte[] bytes) { using (var hash = SHA256.Create()) return hash.ComputeHash(bytes); }
        public Dictionary<string, object> PublicJwk()
        {
            RSAParameters p; lock (key) p = key.ExportParameters(false);
            return new Dictionary<string, object> { { "e", B64(p.Exponent) }, { "kty", "RSA" }, { "n", B64(p.Modulus) } };
        }
        string CanonicalJwk()
        {
            var jwk = PublicJwk();
            return "{\"e\":\"" + jwk["e"] + "\",\"kty\":\"RSA\",\"n\":\"" + jwk["n"] + "\"}";
        }
        public string Proof(string method, string path, string access)
        {
            ValidatePath(path);
            byte[] nonce = new byte[24]; using (var random = RandomNumberGenerator.Create()) random.GetBytes(nonce);
            var header = new Dictionary<string, object> { { "typ", "dpop+jwt" }, { "alg", "RS256" }, { "jwk", PublicJwk() } };
            var claims = new Dictionary<string, object> {
                { "iat", (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds },
                { "jti", B64(nonce) }, { "htm", method.ToUpperInvariant() }, { "htu", new Uri(origin, path).AbsoluteUri }
            };
            if (access != null) claims.Add("ath", B64(Hash(Encoding.ASCII.GetBytes(access))));
            string payload = B64(Encoding.UTF8.GetBytes(json.Serialize(header))) + "." + B64(Encoding.UTF8.GetBytes(json.Serialize(claims)));
            byte[] signature; lock (key) signature = key.SignData(Encoding.ASCII.GetBytes(payload), CryptoConfig.MapNameToOID("SHA256"));
            return payload + "." + B64(signature);
        }
        static void ValidatePath(string path)
        {
            if (path == null || !path.StartsWith("/v1/", StringComparison.Ordinal) || path.Contains("?") || path.Contains("#") ||
                path.Contains("..") || path.Contains("%") || path.Contains("\\")) throw new ArgumentException("Invalid API path.");
        }
        public Dictionary<string, object> Call(string method, string path, object body, bool authenticated)
        {
            ValidatePath(path);
            if (method != "POST" && method != "GET") throw new ArgumentException("Unsupported API method.");
            string access; lock (gate) access = authenticated ? accessToken : null;
            if (authenticated && access == null) throw new InvalidOperationException("Sign in first.");
            var request = (HttpWebRequest)WebRequest.Create(new Uri(origin, path));
            request.Method = method; request.AllowAutoRedirect = false; request.Timeout = 15000; request.ReadWriteTimeout = 15000;
            request.ContentType = "application/json"; request.Accept = "application/json";
            request.Headers["DPoP"] = Proof(method, path, access);
            if (access != null) request.Headers[HttpRequestHeader.Authorization] = "DPoP " + access;
            // Certificate validation is the OS default. No permissive callback or fallback is installed.
            if (method == "POST") {
                byte[] bytes = Encoding.UTF8.GetBytes(json.Serialize(body));
                if (bytes.Length > 16384) throw new ArgumentException("Request too large.");
                request.ContentLength = bytes.Length;
                using (var stream = request.GetRequestStream()) stream.Write(bytes, 0, bytes.Length);
            }
            try {
                using (var response = (HttpWebResponse)request.GetResponse()) {
                    if (response.StatusCode != HttpStatusCode.OK || response.ContentLength > 262144 ||
                        !response.ContentType.StartsWith("application/json", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Unexpected account server response.");
                    using (var stream = response.GetResponseStream()) using (var buffer = new MemoryStream()) {
                        var bytes = new byte[4096]; int n;
                        while ((n = stream.Read(bytes, 0, bytes.Length)) > 0) {
                            if (buffer.Length + n > 262144) throw new InvalidDataException("Account response too large.");
                            buffer.Write(bytes, 0, n);
                        }
                        return json.Deserialize<Dictionary<string, object>>(Encoding.UTF8.GetString(buffer.ToArray()));
                    }
                }
            } catch (WebException e) {
                if (e.Response != null) e.Response.Dispose();
                throw new InvalidOperationException("The family server could not authorize this request. Check your connection and sign-in details.");
            }
        }
        public string Login(string username, string password, string code, bool trustNewViewer)
        {
            var result = Call("POST", "/v1/login", new { username, password, code, trust_new_viewer = trustNewViewer, viewer_label = Environment.MachineName }, false);
            lock (gate) { accessToken = (string)result["access_token"]; SessionId = (string)result["session_id"]; }
            return (string)result["refresh_token"];
        }
        public string Refresh(string refreshToken)
        {
            var result = Call("POST", "/v1/refresh", new { refresh_token = refreshToken }, false);
            lock (gate) { accessToken = (string)result["access_token"]; SessionId = (string)result["session_id"]; }
            return (string)result["refresh_token"];
        }
        public void Forget() { lock (gate) { accessToken = null; SessionId = null; } }
        public void Logout()
        {
            try { if (SessionId != null) Call("POST", "/v1/logins/revoke", new { session_id = SessionId }, true); }
            finally { Forget(); }
        }
    }
}
