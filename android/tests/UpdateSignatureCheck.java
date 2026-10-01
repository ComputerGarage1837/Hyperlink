package ca.myfamilyapps.hyperlink;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.regex.*;
public class UpdateSignatureCheck {
interface Work {void run()throws Exception;}
static void refuse(Work work)throws Exception {try{work.run();throw new AssertionError("Untrusted update accepted");}catch(AssertionError e){throw e;}catch(Exception expected){}}
static String field(String json,String name)throws Exception {Matcher m=Pattern.compile("\""+name+"\"\\s*:\\s*\"([^\"]*)\"").matcher(json);if(!m.find())throw new Exception("Missing fixture field");return m.group(1);}
public static void main(String[] args)throws Exception {
String json=new String(Files.readAllBytes(Paths.get(args[0])),StandardCharsets.UTF_8),payload=field(json,"payload"),signature=field(json,"signature");
byte[] verified=UpdateSignatures.verify(payload,signature);
if(!new String(verified,StandardCharsets.UTF_8).contains("Hyperlink"))throw new AssertionError("Wrong payload");
byte[] altered=verified.clone();altered[altered.length/2]^=1;
refuse(()->UpdateSignatures.verify(Base64.getEncoder().encodeToString(altered),signature));
byte[] bad=Base64.getDecoder().decode(signature);bad[0]^=1;
refuse(()->UpdateSignatures.verify(payload,Base64.getEncoder().encodeToString(bad)));
refuse(()->UpdateSignatures.verify(payload,"AA=="));
refuse(()->UpdateSignatures.verify(new String(new char[12001]).replace('\0','A'),signature));
System.out.println("Android update feed: authentic signature accepted; altered payload/signature, truncated signature and oversized envelope rejected.");
}
}
