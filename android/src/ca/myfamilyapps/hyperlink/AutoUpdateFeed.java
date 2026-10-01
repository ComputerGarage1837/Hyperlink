package ca.myfamilyapps.hyperlink;
import org.json.JSONObject;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.io.IOException;
final class AutoUpdateFeed {
    final int code;final long size;final String hash,version;
    private AutoUpdateFeed(JSONObject data)throws Exception {
        code=data.getInt("androidCode");size=data.getLong("androidSize");hash=data.getString("androidSha256");version=data.getString("version");
        Instant expiry=Instant.parse(data.getString("expires")),now=Instant.now();
        if(!data.getString("product").equals("Hyperlink")||code<1||data.getInt("sequence")<1||size<1||size>32L*1024*1024||!hash.matches("[a-f0-9]{64}")||!version.matches("[0-9]{1,5}(\\.[0-9]{1,5}){2,3}")||!expiry.isAfter(now)||expiry.isAfter(now.plusSeconds(400L*86400)))throw new IOException("Invalid or expired update feed");
    }
    static AutoUpdateFeed verify(String json)throws Exception {
        if(json.getBytes(StandardCharsets.UTF_8).length>16384)throw new IOException("Update feed too large");
        JSONObject envelope=new JSONObject(json);byte[] payload=UpdateSignatures.verify(envelope.getString("payload"),envelope.getString("signature"));
        return new AutoUpdateFeed(new JSONObject(new String(payload,StandardCharsets.UTF_8)));
    }
}
