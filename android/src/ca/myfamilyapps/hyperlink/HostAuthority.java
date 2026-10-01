package ca.myfamilyapps.hyperlink;

import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.security.spec.RSAPublicKeySpec;
import java.util.Base64;

final class HostAuthority {
    static boolean verify(String id,String fingerprint,String code,String attestation){
        try {
            if(!id.matches("[a-f0-9]{64}")||!fingerprint.matches("[a-f0-9]{64}")||!code.matches("[1-9][0-9]{7}"))return false;
            byte[] signature=Base64.getDecoder().decode(attestation);if(signature.length!=384)return false;
            PublicKey key=KeyFactory.getInstance("RSA").generatePublic(new RSAPublicKeySpec(new BigInteger(1,Base64.getDecoder().decode(HostAuthorityKey.MODULUS)),new BigInteger(1,Base64.getDecoder().decode(HostAuthorityKey.EXPONENT))));
            Signature verifier=Signature.getInstance("SHA256withRSA");verifier.initVerify(key);verifier.update(("Hyperlink host/1\n"+id+"\n"+fingerprint+"\n"+code).getBytes(StandardCharsets.UTF_8));return verifier.verify(signature);
        }catch(Exception invalid){return false;}
    }
}
