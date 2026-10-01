package ca.myfamilyapps.hyperlink;

import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.*;
import java.security.interfaces.RSAPublicKey;
import java.util.*;
import java.util.regex.*;
import javax.imageio.ImageIO;
import java.io.*;

/** Loopback fixture only. Controlled test JSON; production uses Android's JSONObject. */
public final class WireCheck {
    private static String text(String json,String field) throws IOException {
        Matcher match=Pattern.compile("\""+field+"\"\\s*:\\s*\"([^\"\\\\]*)\"").matcher(json);
        if(!match.find())throw new IOException("Fixture field missing: "+field);return match.group(1);
    }
    private static int number(String json,String field) throws IOException {
        Matcher match=Pattern.compile("\""+field+"\"\\s*:\\s*(\\d+)").matcher(json);
        if(!match.find())throw new IOException("Fixture integer missing");return Integer.parseInt(match.group(1));
    }
    private static String read(PinnedWire wire) throws Exception {
        PinnedWire.Packet packet=wire.read();if(packet.kind!=1)throw new IOException("Expected control frame");
        return new String(packet.bytes,StandardCharsets.UTF_8);
    }
    private static void send(PinnedWire wire,String json) throws Exception {wire.write(1,json.getBytes(StandardCharsets.UTF_8));}
    private static String invitation(Path file) throws Exception {
        long deadline=System.currentTimeMillis()+90000;
        while(!Files.exists(file)){if(System.currentTimeMillis()>deadline)throw new IOException("Fixture host timed out");Thread.sleep(100);}
        String encoded=new String(Files.readAllBytes(file),StandardCharsets.UTF_8);
        return new String(Base64.getUrlDecoder().decode(encoded.substring(7)),StandardCharsets.UTF_8);
    }
    private static void authenticate(PinnedWire wire,KeyPair pair,String fingerprint,String hello) throws Exception {
        send(wire,hello);String challenge=read(wire);
        if(!text(challenge,"kind").equals("challenge"))throw new IOException("Missing RSA challenge");
        String id=Signing.hex(Signing.hash(Signing.xml((RSAPublicKey)pair.getPublic()).getBytes(StandardCharsets.UTF_8)));
        Signature signer=Signature.getInstance("SHA256withRSA");signer.initSign(pair.getPrivate());
        signer.update(("Hyperlink/1\n"+text(challenge,"nonce")+"\n"+fingerprint+"\n"+id).getBytes(StandardCharsets.UTF_8));
        send(wire,"{\"signature\":\""+Base64.getEncoder().encodeToString(signer.sign())+"\"}");
    }
    private static void session(Path root,boolean readonly) throws Exception {
        String invite=invitation(root.resolve(readonly?"readonly.invite":"control.invite"));
        String address=text(invite,"Address"),fingerprint=text(invite,"Fingerprint");int port=number(invite,"Port");
        if(!address.equals("127.0.0.1"))throw new IOException("Fixture must stay on loopback");
        if(!readonly){boolean denied=false;try(PinnedWire wrong=new PinnedWire(address,port,new String(new char[64]).replace('\0','0'))){throw new AssertionError("Wrong pin accepted");}catch(IOException expected){denied=true;}if(!denied)throw new AssertionError("Wrong pin not denied");}
        KeyPairGenerator gen=KeyPairGenerator.getInstance("RSA");gen.initialize(2048);KeyPair pair=gen.generateKeyPair();
        String xml=Signing.xml((RSAPublicKey)pair.getPublic()),id=Signing.hex(Signing.hash(xml.getBytes(StandardCharsets.UTF_8)));
        String name=readonly?"Java read-only test":"Java interoperability test";
        try(PinnedWire wire=new PinnedWire(address,port,fingerprint)){
            authenticate(wire,pair,fingerprint,"{\"version\":1,\"operation\":\"pair\",\"id\":\""+id+"\",\"publicKey\":\""+xml+"\",\"name\":\""+name+"\",\"code\":\""+text(invite,"Code")+"\"}");
            if(!text(read(wire),"kind").equals("paired"))throw new AssertionError("Pairing failed");
            if(!readonly){Files.write(root.resolve("control.paired"),new byte[]{1});long deadline=System.currentTimeMillis()+10000;
                while(!Files.exists(root.resolve("control.permissions"))){if(System.currentTimeMillis()>deadline)throw new IOException("Owner fixture permissions timed out");Thread.sleep(20);}}
        }
        try(PinnedWire wire=new PinnedWire(address,port,fingerprint)){
            authenticate(wire,pair,fingerprint,"{\"version\":1,\"operation\":\"connect\",\"id\":\""+id+"\"}");
            String accepted=read(wire);if(!text(accepted,"kind").equals("accepted"))throw new AssertionError("Session not accepted");
            if(!readonly)extensions(wire);
            if(!accepted.contains("\"control\":"+(!readonly)))throw new AssertionError("Wrong input permission");
            wire.timeout(8000);PinnedWire.Packet image=wire.read();
            if(image.kind!=10||ImageIO.read(new ByteArrayInputStream(image.bytes))==null)throw new AssertionError("JPEG frame failed");
            send(wire,"{\"kind\":\"input\",\"type\":\"text\",\"text\":\"Family é 😀\"}");
            if(readonly){boolean denied=false;try{while(true)wire.read();}catch(IOException expected){denied=true;}if(!denied)throw new AssertionError("Read-only input accepted");}
            else {send(wire,"{\"kind\":\"ping\"}");for(int n=0;n<10;n++)wire.read();}
        }
        Files.write(root.resolve(readonly?"readonly.done":"control.done"),new byte[]{1});
    }
    private static String request(PinnedWire wire,String operation,String fields) throws Exception {
        String id=java.util.UUID.randomUUID().toString().replace("-","");
        send(wire,"{\"kind\":\"extension\",\"request\":\""+id+"\",\"operation\":\""+operation+"\""+fields+"}");
        for(int n=0;n<300;n++){PinnedWire.Packet packet=wire.read();if(packet.kind!=1)continue;
            String reply=new String(packet.bytes,StandardCharsets.UTF_8);
            if(text(reply,"request").equals(id)){if(!reply.contains("\"ok\":true"))throw new AssertionError("Extension denied: "+operation);return reply;}}
        throw new IOException("Extension reply missing");
    }
    private static void extensions(PinnedWire wire) throws Exception {
        byte[] bytes="Android file integrity fixture".getBytes(StandardCharsets.UTF_8);
        String digest=Signing.hex(Signing.hash(bytes));
        String begin=request(wire,"file-upload-begin",",\"name\":\"android-fixture.txt\",\"size\":"+bytes.length+",\"sha256\":\""+digest+"\"");
        String id=text(begin,"id"),data=Base64.getEncoder().encodeToString(bytes);
        String written=request(wire,"file-upload-write",",\"id\":\""+id+"\",\"offset\":0,\"data\":\""+data+"\"");
        if(number(written,"offset")!=bytes.length)throw new AssertionError("Upload offset mismatch");
        String commit=request(wire,"file-upload-commit",",\"id\":\""+id+"\"");if(!text(commit,"sha256").equals(digest))throw new AssertionError("Upload integrity mismatch");
        begin=request(wire,"file-download-begin",",\"name\":\"android-fixture.txt\"");id=text(begin,"id");
        String downloaded=request(wire,"file-download-read",",\"id\":\""+id+"\",\"offset\":0");
        if(!java.util.Arrays.equals(bytes,Base64.getDecoder().decode(text(downloaded,"data"))) || !text(downloaded,"sha256").equals(digest))throw new AssertionError("Download integrity mismatch");
        request(wire,"file-cancel","");
        request(wire,"clipboard-write",",\"text\":\"Android clipboard fixture\"");
        if(!text(request(wire,"clipboard-read",""),"text").equals("Android clipboard fixture"))throw new AssertionError("Directional clipboard mismatch");
        String audio=request(wire,"audio-start","");if(number(audio,"sampleRate")!=48000 || number(audio,"channels")!=2 || number(audio,"bits")!=16)throw new AssertionError("Audio descriptor mismatch");
        boolean received=false;for(int n=0;n<100;n++){PinnedWire.Packet packet=wire.read();if(packet.kind==11){if(packet.bytes.length!=3856)throw new AssertionError("Audio frame bounds");received=true;break;}}
        if(!received)throw new AssertionError("Audio packet missing");request(wire,"audio-stop","");
        System.out.println("Java / Windows encrypted file, clipboard and synthetic audio checks passed");
    }
    public static void main(String[] args) throws Exception {Path root=Paths.get(args[0]);session(root,false);session(root,true);System.out.println("Java / Windows wire interoperability passed.");}
}
