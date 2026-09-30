package ca.myfamilyapps.hyperlink;

import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.security.interfaces.RSAPublicKey;
import java.util.Arrays;
import java.util.Base64;

/** Standard JCA RSA/SHA-256 signatures and RFC 7638/9449 representations. */
public final class Signing {
    private Signing() {}
    public static String b64(byte[] value) { return Base64.getUrlEncoder().withoutPadding().encodeToString(value); }
    public static byte[] unsigned(BigInteger value) {
        byte[] bytes = value.toByteArray();
        return bytes.length > 1 && bytes[0] == 0 ? Arrays.copyOfRange(bytes, 1, bytes.length) : bytes;
    }
    public static byte[] hash(byte[] value) throws GeneralSecurityException { return MessageDigest.getInstance("SHA-256").digest(value); }
    public static String hex(byte[] value) {
        StringBuilder out = new StringBuilder();
        for (byte b : value) out.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return out.toString();
    }
    public static String jwk(RSAPublicKey key) {
        if (key.getModulus().bitLength() < 2048 || key.getModulus().bitLength() > 4096) throw new IllegalArgumentException("Unsupported identity key");
        return "{\"e\":\""+b64(unsigned(key.getPublicExponent()))+"\",\"kty\":\"RSA\",\"n\":\""+b64(unsigned(key.getModulus()))+"\"}";
    }
    public static String thumbprint(RSAPublicKey key) throws GeneralSecurityException { return b64(hash(jwk(key).getBytes(StandardCharsets.UTF_8))); }
    public static String xml(RSAPublicKey key) {
        return "<RSAKeyValue><Modulus>"+Base64.getEncoder().encodeToString(unsigned(key.getModulus()))+
            "</Modulus><Exponent>"+Base64.getEncoder().encodeToString(unsigned(key.getPublicExponent()))+"</Exponent></RSAKeyValue>";
    }
    public static byte[] sign(PrivateKey key, byte[] data) throws GeneralSecurityException {
        Signature signer = Signature.getInstance("SHA256withRSA"); signer.initSign(key); signer.update(data); return signer.sign();
    }
    public static String dpop(PrivateKey key, RSAPublicKey pub, String origin, String method, String path, String access) throws GeneralSecurityException {
        if (!origin.matches("https://[a-z0-9.-]+(?::[0-9]{1,5})?") || (!method.equals("GET") && !method.equals("POST")) ||
            !path.matches("/v1/[a-z-]+(?:/[a-z-]+)*")) throw new IllegalArgumentException("Invalid API endpoint");
        byte[] nonce = new byte[24]; new SecureRandom().nextBytes(nonce);
        String header = "{\"typ\":\"dpop+jwt\",\"alg\":\"RS256\",\"jwk\":"+jwk(pub)+"}";
        String claims = "{\"iat\":"+(System.currentTimeMillis()/1000)+",\"jti\":\""+b64(nonce)+"\",\"htm\":\""+method+
            "\",\"htu\":\""+origin+path+"\"";
        if (access != null) claims += ",\"ath\":\""+b64(hash(access.getBytes(StandardCharsets.US_ASCII)))+"\"";
        claims += "}";
        String data = b64(header.getBytes(StandardCharsets.UTF_8))+"."+b64(claims.getBytes(StandardCharsets.UTF_8));
        return data+"."+b64(sign(key, data.getBytes(StandardCharsets.US_ASCII)));
    }
}
