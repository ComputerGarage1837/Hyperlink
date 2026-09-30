package ca.myfamilyapps.hyperlink;

import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.*;
import java.security.interfaces.RSAPublicKey;
import java.util.Base64;

/** Tests the same JCA signing implementation shipped in the Android APK. */
public final class SigningCheck {
    public static void main(String[] args) throws Exception {
        KeyPairGenerator generator=KeyPairGenerator.getInstance("RSA");generator.initialize(2048);
        KeyPair pair=generator.generateKeyPair();RSAPublicKey pub=(RSAPublicKey)pair.getPublic();
        String access=new String(new char[43]).replace('\0','a');
        String proof=Signing.dpop(pair.getPrivate(),pub,"https://hyperlink.test","POST","/v1/devices",access);
        String[] jwt=proof.split("\\.");
        if(jwt.length!=3)throw new AssertionError("Malformed proof");
        Signature verify=Signature.getInstance("SHA256withRSA");verify.initVerify(pub);
        verify.update((jwt[0]+"."+jwt[1]).getBytes(StandardCharsets.US_ASCII));
        if(!verify.verify(Base64.getUrlDecoder().decode(jwt[2])))throw new AssertionError("Proof signature failed");
        String payload=new String(Base64.getUrlDecoder().decode(jwt[1]),StandardCharsets.UTF_8);
        if(!payload.contains("\"ath\":\""+Signing.b64(Signing.hash(access.getBytes(StandardCharsets.US_ASCII)))+"\""))throw new AssertionError("Access binding failed");
        String xml=Signing.xml(pub);
        if(!xml.startsWith("<RSAKeyValue><Modulus>")||!xml.endsWith("</Exponent></RSAKeyValue>"))throw new AssertionError("Windows key representation failed");
        String jkt=Signing.thumbprint(pub);
        if(!jkt.matches("[A-Za-z0-9_-]{43}"))throw new AssertionError("Thumbprint failed");
        if(args.length==1)Files.write(Paths.get(args[0]),("{\"proof\":\""+proof+"\",\"jkt\":\""+jkt+"\"}").getBytes(StandardCharsets.UTF_8));
        System.out.println("4 Android signing checks passed; controller fixture generated.");
    }
}
