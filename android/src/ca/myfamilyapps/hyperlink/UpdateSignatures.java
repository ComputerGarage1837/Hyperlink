package ca.myfamilyapps.hyperlink;
import java.io.IOException;
import java.math.BigInteger;
import java.security.*;
import java.security.spec.RSAPublicKeySpec;
import java.util.Base64;
final class UpdateSignatures {
    static byte[] verify(String payloadText,String signatureText)throws Exception {
        if(payloadText.length()>12000||signatureText.length()!=512)throw new IOException("Invalid update envelope");
        byte[] payload=Base64.getDecoder().decode(payloadText),signature=Base64.getDecoder().decode(signatureText);
        if(payload.length>8192||signature.length!=384)throw new IOException("Invalid update signature");
        RSAPublicKeySpec spec=new RSAPublicKeySpec(new BigInteger(1,Base64.getDecoder().decode(UpdateReleaseKey.MODULUS)),new BigInteger(1,Base64.getDecoder().decode(UpdateReleaseKey.EXPONENT)));
        Signature verifier=Signature.getInstance("SHA256withRSA");verifier.initVerify(KeyFactory.getInstance("RSA").generatePublic(spec));verifier.update(payload);
        if(!verifier.verify(signature))throw new IOException("Untrusted update feed");return payload;
    }
}
