package ca.myfamilyapps.hyperlink;

import android.app.*;
import android.content.*;
import android.graphics.Color;
import android.os.*;
import android.text.InputType;
import android.view.*;
import android.widget.*;
import java.util.concurrent.*;
import org.json.*;

public final class MainActivity extends Activity {
    private AutoUpdates updates;
    static final int BACKGROUND = Color.rgb(17,25,37), TEXT = Color.rgb(228,236,247), MINT = Color.rgb(109,231,205);
    private Identity identity; private Api api;
    private TextView status; private LinearLayout peers, family;
    private EditText username, password, code;
    private CheckBox trust, remember;
    private boolean busy;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    interface Job { void run() throws Exception; }
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout root = root(this); ScrollView scroll = new ScrollView(this); LinearLayout body = column(this);
        scroll.addView(body); root.addView(scroll, new LinearLayout.LayoutParams(-1,-1)); setContentView(root);
        label(body,"Hyperlink",32,MINT); label(body,"Private family remote desktop",16,TEXT);
        updates=new AutoUpdates(this,body);
        status = label(body,"Preparing your protected viewer identity…",14,TEXT);
        label(body,"Connect to a computer",22,TEXT);
        label(body,"On Windows, open Unattended access, choose a PIN, and use the computer ID shown there.",14,TEXT);
        final EditText computerId=field(body,"Computer ID (8 digits)",false);computerId.setInputType(InputType.TYPE_CLASS_NUMBER);computerId.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(8)});
        final EditText connectionPin=field(body,"Windows PIN",true);connectionPin.setInputType(InputType.TYPE_CLASS_NUMBER|InputType.TYPE_NUMBER_VARIATION_PASSWORD);connectionPin.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(12)});
        button(body,"Connect",() -> {if(identity==null||busy)return;String id=computerId.getText().toString().trim(),pin=connectionPin.getText().toString();connectionPin.setText("");connectByPin(id,pin);});
        label(body,"Saved computers",20,TEXT);
        button(body,"Wake a computer",() -> WakeDialog.show(this));
        button(body,"Recordings",() -> startActivity(new Intent(this,RecordingsActivity.class)));
        peers = column(this); body.addView(peers);
        final LinearLayout accountBody=column(this);accountBody.setVisibility(android.view.View.GONE);
        button(body,"Advanced / family account",() -> accountBody.setVisibility(accountBody.getVisibility()==android.view.View.GONE?android.view.View.VISIBLE:android.view.View.GONE));
        body.addView(accountBody);
        button(accountBody,"Manual invitation (attended LAN)",this::pair);
        label(accountBody,"Family account",22,TEXT); label(accountBody,"hyperlink.myfamilyapps.ca",14,MINT);
        username = field(accountBody,"Username",false); password = field(accountBody,"Password",true); code = field(accountBody,"Authenticator code",false);
        code.setInputType(InputType.TYPE_CLASS_NUMBER);
        trust = new CheckBox(this); trust.setText("Approve this phone as a new trusted viewer"); accountBody.addView(trust);
        remember = new CheckBox(this); remember.setText("Remember login securely on this phone"); accountBody.addView(remember);
        button(accountBody,"Sign in",() -> {
            String name=username.getText().toString(), pass=password.getText().toString(), otp=code.getText().toString();
            boolean approve=trust.isChecked(), save=remember.isChecked(); password.setText(""); code.setText("");
            run(() -> { JSONObject result=api.call("POST","/v1/login",Json.object("username",name,"password",pass,"code",otp,
                "trust_new_viewer",approve,"viewer_label",deviceName()),false); api.acceptLogin(result); identity.remember(save?result.getString("refresh_token"):null); },this::loadFamily);
        });
        button(accountBody,"Restore remembered login",() -> run(() -> {
            String token=identity.remembered(); if(token==null) throw new Exception("No remembered login. Sign in first.");
            JSONObject result=api.call("POST","/v1/refresh",Json.object("refresh_token",token),false); api.acceptLogin(result); identity.remember(result.getString("refresh_token"));
        },this::loadFamily));
        button(accountBody,"Accept family invitation",() -> prompt("Accept invitation","Your personal invitation",true,invite -> {
            String name=username.getText().toString(), pass=password.getText().toString(); password.setText(""); final JSONObject[] result={null};
            run(() -> result[0]=api.call("POST","/v1/register",Json.object("invite",invite,"username",name,"password",pass),false),() -> activate(result[0]));
        }));
        button(accountBody,"Recover account",() -> prompt("Recover account","One unused recovery code. This revokes old logins and trust.",true,recovery -> {
            String name=username.getText().toString(), pass=password.getText().toString(); password.setText(""); final JSONObject[] result={null};
            run(() -> { result[0]=api.call("POST","/v1/recover",Json.object("username",name,"password",pass,"recovery_code",recovery),false); api.forget(); identity.remember(null); },() -> activate(result[0]));
        }));
        button(accountBody,"Refresh family computers",this::loadFamily);
        button(accountBody,"Verify MFA again",() -> prompt("Verify account","Current authenticator code",false,otp -> run(() -> api.call("POST","/v1/elevate",Json.object("code",otp),true),() -> status.setText("MFA verified for five minutes."))));
        button(accountBody,"Trusted viewers",this::viewers);
        button(accountBody,"Account logins",this::logins);
        button(accountBody,"Sign out",() -> run(() -> {
            identity.remember(null);
            try { if(api.session!=null) api.call("POST","/v1/logins/revoke",Json.object("session_id",api.session),true); }
            finally { api.forget(); }
        },() -> { family.removeAllViews(); status.setText("Signed out. Remembered login removed."); }));
        family=column(this); accountBody.addView(family);

        run(() -> { identity=new Identity(getApplicationContext()); api=new Api(identity); },() -> { status.setText("Ready. Enter the computer ID and PIN from Windows."); loadPeers(); });
    }
    static String deviceName() { String name=(Build.MANUFACTURER+" "+Build.MODEL).trim(); return name.substring(0,Math.min(48,name.length())); }
    static int dp(Context c,int value) { return Math.round(value*c.getResources().getDisplayMetrics().density); }
    static LinearLayout column(Context c) { LinearLayout layout=new LinearLayout(c); layout.setOrientation(LinearLayout.VERTICAL); return layout; }
    static LinearLayout root(Activity activity) {
        LinearLayout root=column(activity); root.setBackgroundColor(BACKGROUND);
        int margin=dp(activity,16); root.setPadding(margin,margin,margin,margin);
        root.setOnApplyWindowInsetsListener((view,insets) -> {
            int left=insets.getSystemWindowInsetLeft(),top=insets.getSystemWindowInsetTop(),right=insets.getSystemWindowInsetRight(),bottom=insets.getSystemWindowInsetBottom();
            if(Build.VERSION.SDK_INT>=28 && insets.getDisplayCutout()!=null) { left=Math.max(left,insets.getDisplayCutout().getSafeInsetLeft()); right=Math.max(right,insets.getDisplayCutout().getSafeInsetRight()); }
            view.setPadding(margin+left,margin+top,margin+right,margin+bottom); return insets;
        }); return root;
    }
    static TextView label(LinearLayout parent,String text,int size,int color) {
        TextView value=new TextView(parent.getContext()); value.setText(text); value.setTextSize(size); value.setTextColor(color);
        value.setPadding(0,dp(parent.getContext(),8),0,dp(parent.getContext(),8)); parent.addView(value); return value;
    }
    static EditText field(LinearLayout parent,String hint,boolean secret) {
        EditText value=new EditText(parent.getContext()); value.setHint(hint); value.setSingleLine(true);
        value.setInputType(InputType.TYPE_CLASS_TEXT|(secret?InputType.TYPE_TEXT_VARIATION_PASSWORD:InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS)); parent.addView(value); return value;
    }
    static Button button(LinearLayout parent,String text,Runnable action) {
        Button value=new Button(parent.getContext()); value.setText(text); value.setAllCaps(false); value.setOnClickListener(view -> action.run()); parent.addView(value); return value;
    }
    void run(Job job,Runnable complete) {
        if(busy)return; busy=true; status.setText("Working…");
        worker.execute(() -> { String failure=null; try { job.run(); } catch(Exception e) { failure=e.getMessage(); } final String error=failure;
            runOnUiThread(() -> { if(isFinishing()||isDestroyed())return; busy=false; if(error!=null)status.setText(error); else { status.setText("Ready"); complete.run(); } });
        });
    }
    private JSONArray savedPeers() throws Exception {
        String saved=getSharedPreferences("peers",0).getString("list","[]");
        if(saved.length()>32768)throw new Exception("Saved computer list is invalid"); return new JSONArray(saved);
    }
    private void loadPeers() {
        peers.removeAllViews();
        try { JSONArray list=savedPeers();
            for(int i=0;i<list.length();i++) { JSONObject peer=list.getJSONObject(i); final int index=i;
                label(peers,peer.optString("Name","Computer"),18,TEXT);
                button(peers,"Connect",() -> { if(identity==null||busy)return; Intent intent=new Intent(this,ViewerActivity.class); intent.putExtra("peer",peer.toString()); startActivity(intent); });
                button(peers,"Remove saved computer",() -> { try { JSONArray next=new JSONArray(); for(int j=0;j<list.length();j++)if(j!=index)next.put(list.get(j)); getSharedPreferences("peers",0).edit().putString("list",next.toString()).commit(); loadPeers(); }catch(Exception e){status.setText("Could not update saved computers");} });
            }
        }catch(Exception e){status.setText("Saved computers could not be read");}
    }
    private void connectByPin(String code,String pin){
        final JSONObject[] result=new JSONObject[1];
        run(() -> {
            JSONObject peer=Remote.pairPin(identity,code,pin,deviceName());JSONArray list=savedPeers(),next=new JSONArray();
            for(int i=0;i<list.length();i++)if(!list.getJSONObject(i).getString("Id").equals(peer.getString("Id")))next.put(list.get(i));
            if(next.length()>=32)throw new Exception("Saved computer limit reached");next.put(peer);
            if(!getSharedPreferences("peers",0).edit().putString("list",next.toString()).commit())throw new Exception("Could not save this computer");result[0]=peer;
        },() -> {loadPeers();status.setText("Computer authorized. Connecting…");Intent intent=new Intent(this,ViewerActivity.class);intent.putExtra("peer",result[0].toString());startActivity(intent);});
    }
    private void pair() {
        if(identity==null||busy)return;
        prompt("Add computer","Paste the five-minute invitation from the Windows host",true,text -> run(() -> {
            JSONObject peer=Remote.pair(identity,Remote.invitation(text),deviceName()); JSONArray list=savedPeers(),next=new JSONArray();
            for(int i=0;i<list.length();i++)if(!list.getJSONObject(i).getString("Id").equals(peer.getString("Id")))next.put(list.get(i));
            if(next.length()>=32)throw new Exception("Remove a saved computer before adding another"); next.put(peer);
            if(!getSharedPreferences("peers",0).edit().putString("list",next.toString()).commit())throw new Exception("Could not save computer");
        },() -> {status.setText("Paired. Connect when the owner is ready to approve your session.");loadPeers();}));
    }
    private void loadFamily() {
        final JSONObject[] result={null}; run(() -> result[0]=api.call("GET","/v1/devices",null,true),() -> {
            family.removeAllViews(); try { JSONArray list=result[0].getJSONArray("devices");
                for(int i=0;i<list.length();i++)label(family,list.getJSONObject(i).getString("name")+" · Hosted connection pending",16,TEXT);
                status.setText(list.length()+" authorized family computer(s)");
            }catch(JSONException e){status.setText("Invalid computer list");}
        });
    }
    private void activate(JSONObject enrollment) {
        secret("Add Hyperlink to your authenticator","Secret: "+enrollment.optString("secret")+"\n\n"+enrollment.optString("uri")+"\n\nEnrollment expires in ten minutes.",() ->
            prompt("Complete enrollment","Code from your authenticator",false,otp -> { final JSONObject[] result={null};
                run(() -> result[0]=api.call("POST","/v1/activate",Json.object("enrollment_id",enrollment.getString("enrollment_id"),"code",otp),false),() -> {
                    StringBuilder codes=new StringBuilder(); JSONArray values=result[0].optJSONArray("recovery_codes");
                    if(values!=null)for(int i=0;i<values.length();i++)codes.append(values.optString(i)).append('\n');
                    secret("Save recovery codes privately",codes.toString(),() -> status.setText("Account ready. Use the next authenticator code to sign in."));
                });
            }));
    }
    private void viewers() {
        final JSONObject[] result={null}; run(() -> result[0]=api.call("GET","/v1/viewers",null,true),() -> {
            StringBuilder text=new StringBuilder(); JSONArray list=result[0].optJSONArray("viewers");
            if(list!=null)for(int i=0;i<list.length();i++){JSONObject v=list.optJSONObject(i);if(v!=null)text.append(v.optString("label")).append("\n").append(v.optString("id")).append(" · revoked=").append(v.optInt("revoked")).append("\n\n");}
            secret("Trusted viewers",text.toString(),() -> prompt("Revoke viewer","Viewer ID. Every login using that key will be revoked.",false,id -> run(() -> api.call("POST","/v1/viewers/revoke",Json.object("viewer_id",id),true),() -> status.setText("Viewer revoked."))));
        });
    }
    private void logins() {
        final JSONObject[] result={null}; run(() -> result[0]=api.call("GET","/v1/logins",null,true),() -> {
            StringBuilder text=new StringBuilder(); JSONArray list=result[0].optJSONArray("logins");
            if(list!=null)for(int i=0;i<list.length();i++){JSONObject s=list.optJSONObject(i);if(s!=null)text.append(s.optString("id")).append(" · revoked=").append(s.optInt("revoked")).append("\n\n");}
            secret("Your logins",text.toString(),() -> prompt("Revoke login","Login ID, or cancel",false,id -> run(() -> api.call("POST","/v1/logins/revoke",Json.object("session_id",id),true),() -> {if(id.equals(api.session))api.forget();status.setText("Login revoked.");})));
        });
    }
    interface Answer { void accept(String value); }
    private void prompt(String title,String hint,boolean hidden,Answer answer) {
        if(busy)return; EditText field=new EditText(this); field.setHint(hint); field.setSingleLine(false); field.setMaxLines(5);
        field.setInputType(InputType.TYPE_CLASS_TEXT|(hidden?InputType.TYPE_TEXT_VARIATION_PASSWORD:InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS));
        new AlertDialog.Builder(this).setTitle(title).setView(field).setPositiveButton("Continue",(dialog,which) -> answer.accept(field.getText().toString().trim())).setNegativeButton("Cancel",null).show();
    }
    private void secret(String title,String text,Runnable next) {
        TextView value=new TextView(this); value.setText(text); value.setTextIsSelectable(true); value.setPadding(dp(this,20),dp(this,12),dp(this,20),dp(this,12));
        ScrollView scroll=new ScrollView(this); scroll.addView(value);
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle(title).setView(scroll).setPositiveButton("Continue",(d,w)->next.run()).create();
        dialog.getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE); dialog.show();
    }
    protected void onResume(){super.onResume();if(updates!=null)updates.check();}
    protected void onDestroy(){if(updates!=null)updates.close();worker.shutdownNow();super.onDestroy();}
}
