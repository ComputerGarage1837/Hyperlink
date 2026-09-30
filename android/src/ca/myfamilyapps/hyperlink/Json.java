package ca.myfamilyapps.hyperlink;

import java.io.IOException;
import java.nio.*;
import java.nio.charset.*;
import org.json.*;

final class Json {
    private Json() {}
    static JSONObject object(Object... fields) throws JSONException {
        JSONObject value = new JSONObject();
        for (int i = 0; i < fields.length; i += 2) value.put((String)fields[i], fields[i+1]);
        return value;
    }
    static JSONObject parse(byte[] bytes, int limit) throws IOException, JSONException {
        if (bytes.length > limit) throw new IOException("Response too large");
        String text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
        int depth = 0; boolean quoted = false, escape = false;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (quoted) { if (escape) escape = false; else if (c == '\\') escape = true; else if (c == '"') quoted = false; }
            else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') { if (++depth > 8) throw new IOException("Response nesting too deep"); }
            else if (c == '}' || c == ']') { if (--depth < 0) throw new IOException("Invalid response"); }
        }
        if (depth != 0 || quoted) throw new IOException("Incomplete response");
        return new JSONObject(text);
    }
}
