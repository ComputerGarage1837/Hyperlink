package ca.myfamilyapps.hyperlink;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;
final class RelayClient {
    private static JSONObject reply(RelayTransport socket)throws Exception {
        JSONObject response=Json.parse(socket.readText().getBytes(StandardCharsets.UTF_8),16384);
        if(response.optString("kind").equals("error"))throw new IOException("Computer unavailable. Check that unattended access is enabled on Windows.");
        return response;
    }
    static JSONObject lookup(String code)throws Exception {
        if(code==null||!code.matches("[1-9][0-9]{7}"))throw new IOException("Enter the 8-digit computer ID shown on Windows");
        try(RelayTransport socket=RelayTransport.open("control")){
            socket.sendText(Json.object("operation","lookup","code",code).toString());JSONObject target=reply(socket);
            if(!target.optString("kind").equals("computer")||!target.optString("code").equals(code)||!HostAuthority.verify(target.optString("id"),target.optString("fingerprint"),code,target.optString("attestation")))throw new IOException("This computer's identity could not be verified");
            return target;
        }
    }
    static PinnedWire open(JSONObject target)throws Exception {
        RelayTransport transport=RelayTransport.open("session");
        try{
            transport.sendText(Json.object("session",target.getString("session"),"token",target.getString("token")).toString());
            if(!reply(transport).optString("kind").equals("ready"))throw new IOException("Relay connection refused");
            RelayTls tls=new RelayTls(transport,target.getString("fingerprint"));return new PinnedWire(tls.input,tls.output,tls);
        }catch(Exception failure){transport.close();throw failure;}
    }
    static PinnedWire connect(JSONObject peer)throws Exception {
        JSONObject target=lookup(peer.getString("Code"));
        if(!target.getString("id").equals(peer.getString("Id"))||!target.getString("fingerprint").equals(peer.getString("Fingerprint")))throw new IOException("The computer's identity changed. Set it up again from Windows.");
        return open(target);
    }
}
