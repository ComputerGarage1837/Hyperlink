package ca.myfamilyapps.hyperlink;

import java.io.*;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import javax.net.ssl.HttpsURLConnection;
import org.json.*;

final class Api {
    static final String ORIGIN = "https://hyperlink.myfamilyapps.ca";
    private final Identity identity;
    String access, session;
    Api(Identity identity) { this.identity = identity; }
    JSONObject call(String method, String path, JSONObject body, boolean authenticated) throws Exception {
        String token = authenticated ? access : null;
        if (authenticated && token == null) throw new IOException("Sign in first");
        HttpsURLConnection request = (HttpsURLConnection)new URL(ORIGIN+path).openConnection();
        request.setRequestMethod(method); request.setInstanceFollowRedirects(false); request.setConnectTimeout(15000); request.setReadTimeout(15000);
        request.setRequestProperty("Content-Type", "application/json"); request.setRequestProperty("Accept", "application/json");
        request.setRequestProperty("DPoP", Signing.dpop(identity.privateKey, identity.publicKey, ORIGIN, method, path, token));
        if (token != null) request.setRequestProperty("Authorization", "DPoP "+token);
        // Platform CA and hostname validation are retained. No permissive TLS callback.
        try {
            if (body != null) {
                byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8); if (bytes.length > 16384) throw new IOException("Request too large");
                request.setDoOutput(true); request.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream stream = request.getOutputStream()) { stream.write(bytes); }
            }
            if (request.getResponseCode() != 200) throw new IOException("The family service could not authorize this request. Check sign-in, viewer approval and MFA.");
            String contentType = request.getContentType();
            if (contentType == null || !contentType.toLowerCase(java.util.Locale.ROOT).startsWith("application/json")) throw new IOException("Unexpected family service response");
            try (InputStream stream = request.getInputStream(); ByteArrayOutputStream buffer = new ByteArrayOutputStream()) {
                byte[] bytes = new byte[4096]; int count;
                while ((count = stream.read(bytes)) >= 0) { if (buffer.size()+count > 262144) throw new IOException("Response too large"); buffer.write(bytes, 0, count); }
                return Json.parse(buffer.toByteArray(), 262144);
            }
        } finally { request.disconnect(); }
    }
    void acceptLogin(JSONObject result) throws JSONException { access = result.getString("access_token"); session = result.getString("session_id"); }
    void forget() { access = null; session = null; }
}
