package ca.myfamilyapps.hyperlink;

import android.content.Context;
import android.security.keystore.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.security.interfaces.RSAPublicKey;
import java.util.Base64;
import javax.crypto.*;
import javax.crypto.spec.GCMParameterSpec;

final class Identity {
    private static final String RSA = "Hyperlink.viewer.rsa.v1", AES = "Hyperlink.login.aes.v1";
    final PrivateKey privateKey; final RSAPublicKey publicKey;
    final String xml, id, jkt;
    private final Context context;
    Identity(Context context) throws GeneralSecurityException, java.io.IOException {
        this.context = context;
        synchronized (Identity.class) {
        KeyStore keys = KeyStore.getInstance("AndroidKeyStore"); keys.load(null);
        if (!keys.containsAlias(RSA)) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(RSA, KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                .setKeySize(2048).setDigests(KeyProperties.DIGEST_SHA256).setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1).build());
            generator.generateKeyPair();
        }
        privateKey = (PrivateKey)keys.getKey(RSA, null); publicKey = (RSAPublicKey)keys.getCertificate(RSA).getPublicKey();
        xml = Signing.xml(publicKey); id = Signing.hex(Signing.hash(xml.getBytes(StandardCharsets.UTF_8))); jkt = Signing.thumbprint(publicKey);
        }
    }
    private SecretKey loginKey() throws GeneralSecurityException, java.io.IOException {
        KeyStore keys = KeyStore.getInstance("AndroidKeyStore"); keys.load(null);
        if (!keys.containsAlias(AES)) {
            KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(AES, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
            generator.generateKey();
        }
        return (SecretKey)keys.getKey(AES, null);
    }
    synchronized void remember(String refresh) throws GeneralSecurityException, java.io.IOException {
        if (refresh == null) { if (!context.getSharedPreferences("login", 0).edit().clear().commit()) throw new java.io.IOException("Cannot clear saved login"); return; }
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding"); cipher.init(Cipher.ENCRYPT_MODE, loginKey());
        cipher.updateAAD((Api.ORIGIN+"|"+jkt).getBytes(StandardCharsets.UTF_8));
        byte[] encrypted = cipher.doFinal(refresh.getBytes(StandardCharsets.UTF_8));
        byte[] stored = new byte[12+encrypted.length];
        if (cipher.getIV().length != 12) throw new GeneralSecurityException("Unsupported login IV");
        System.arraycopy(cipher.getIV(), 0, stored, 0, 12); System.arraycopy(encrypted, 0, stored, 12, encrypted.length);
        if (!context.getSharedPreferences("login", 0).edit().putString("refresh", Base64.getEncoder().encodeToString(stored)).commit()) throw new java.io.IOException("Cannot store login");
    }
    synchronized String remembered() throws GeneralSecurityException, java.io.IOException {
        String saved = context.getSharedPreferences("login", 0).getString("refresh", null); if (saved == null) return null;
        byte[] bytes = Base64.getDecoder().decode(saved); if (bytes.length < 29 || bytes.length > 1024) throw new GeneralSecurityException("Invalid remembered login");
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding"); cipher.init(Cipher.DECRYPT_MODE, loginKey(), new GCMParameterSpec(128, bytes, 0, 12));
        cipher.updateAAD((Api.ORIGIN+"|"+jkt).getBytes(StandardCharsets.UTF_8));
        return new String(cipher.doFinal(bytes, 12, bytes.length-12), StandardCharsets.UTF_8);
    }
}
