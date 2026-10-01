package ca.myfamilyapps.hyperlink;

import android.app.*;
import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.text.InputFilter;
import android.widget.*;
import java.io.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.*;

/** Explicit, directional session tools. The host independently authorizes every request. */
final class SessionTools implements AutoCloseable {
    static final int UPLOAD=7301, DOWNLOAD_FOLDER=7302;
    private static final long MAX_FILE=1024L*1024*1024;
    private final Activity activity;
    private final Remote remote;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final ExecutorService worker=Executors.newSingleThreadExecutor(task -> {Thread t=new Thread(task,"Hyperlink session tools");t.setDaemon(true);return t;});
    private final AtomicBoolean cancelled=new AtomicBoolean();
    private final Dialog dialog;
    private final TextView status;
    private final EditText text;
    private final LinearLayout actions;
    private volatile boolean closed;
    private boolean busy, picking;
    private String downloadName;
    private volatile SessionAudio audio;
    private volatile boolean privacy;
    private volatile MjpegRecording recording;
    SessionTools(Activity activity,Remote remote){
        this.activity=activity;this.remote=remote;
        dialog=new Dialog(activity);dialog.setTitle("Session tools");
        ScrollView scroll=new ScrollView(activity);LinearLayout body=new LinearLayout(activity);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(24,16,24,16);scroll.addView(body);
        TextView explanation=new TextView(activity);explanation.setText("Each feature needs the host owner's permission. Clipboard exchange is manual. Files are verified before publication; existing names are never replaced.");body.addView(explanation);
        status=new TextView(activity);status.setText("Ready");body.addView(status);
        actions=new LinearLayout(activity);actions.setOrientation(LinearLayout.VERTICAL);body.addView(actions);
        button("Upload a document",() -> pick(UPLOAD));button("Download a host file",this::listFiles);
        text=new EditText(activity);text.setHint("Plain text · maximum 1,024 characters");text.setMinLines(3);text.setFilters(new InputFilter[]{new InputFilter.LengthFilter(1024)});body.addView(text);
        button("Send this text to host clipboard",() -> {String value=text.getText().toString();run(() -> {checkText(value);remote.extension("clipboard-write","text",value);postStatus("Text sent to host clipboard");});});
        button("Read host clipboard into this box",() -> run(() -> {String value=remote.extension("clipboard-read").getString("text");checkText(value);main.post(() -> {if(!closed)text.setText(value);});postStatus("Received text. It has not been copied automatically.");}));
        button("Copy box to Android clipboard",() -> {String value=text.getText().toString();try{checkText(value);ClipboardManager clipboard=(ClipboardManager)activity.getSystemService(Context.CLIPBOARD_SERVICE);clipboard.setPrimaryClip(ClipData.newPlainText("Hyperlink",value));postStatus("Copied to Android clipboard");}catch(Exception e){postStatus("Could not copy text");}});
        button("Start host playback audio",() -> run(this::startAudio));
        button("Stop audio",() -> run(this::stopAudio));
        button("Record video on this phone",() -> run(this::startRecording));
        button("Stop and save recording",() -> {MjpegRecording capture=recording;if(capture!=null){capture.close();postStatus("Saving recording…");}});
        button("Hide host displays",() -> new AlertDialog.Builder(activity).setTitle("Local display privacy").setMessage("Requires the owner's separate permission. The local owner can press Ctrl+Alt+Shift+H to restore the displays and disconnect. Privacy stops after 30 minutes or when session tools close.").setPositiveButton("Start",(d,w) -> run(() -> {if(privacy)throw new IOException("Privacy is already active");remote.extension("privacy-start");privacy=true;if(closed){remote.extension("privacy-stop");privacy=false;}else postStatus("Host display privacy is active");})).setNegativeButton("Cancel",null).show());
        button("Restore host displays",() -> run(() -> {remote.extension("privacy-stop");privacy=false;postStatus("Host displays restored");}));
        Button cancel=new Button(activity);cancel.setText("Cancel file operation");cancel.setOnClickListener(v -> {cancelled.set(true);postStatus("Cancelling; already committed files remain saved.");});body.addView(cancel);
        Button close=new Button(activity);close.setText("Close tools");close.setOnClickListener(v -> dialog.dismiss());body.addView(close);
        dialog.setContentView(scroll);dialog.setOnDismissListener(ignored -> close());
    }
    void show(){dialog.show();dialog.getWindow().setLayout(-1,-2);}
    boolean isClosed(){return closed;}
    boolean isPicking(){return picking;}
    private void button(String label,Runnable action){Button button=new Button(activity);button.setText(label);button.setOnClickListener(v -> action.run());actions.addView(button);}
    private void startRecording() throws Exception {
        if(recording!=null || remote.recording.get()!=null)throw new IOException("Recording is already active or saving");
        boolean granted=false;MjpegRecording capture=null;
        try {
            remote.extension("recording-start");granted=true;
            if(closed){remote.extension("recording-stop");return;}
            capture=new MjpegRecording(new File(activity.getFilesDir(),"recordings"),remote.width,remote.height);
            if(!remote.attachRecording(capture))throw new IOException("The session ended before recording started");
            recording=capture;final MjpegRecording current=capture;
            capture.completion.whenComplete((file,error) -> {
                if(closed)return;
                try {worker.execute(() -> {
                    try{remote.extension("recording-stop");}catch(Exception ex){remote.close();}
                    remote.recording.compareAndSet(current,null);if(recording==current)recording=null;
                    postStatus(error==null?(current.storageLimit?"Recording stopped because of its size limit or storage speed. ":"")+"Saved locally. Open Recordings from the home screen to export.":"Recording failed; no incomplete clip was published.");
                });}catch(RejectedExecutionException ex){if(!closed)remote.close();}
            });
            if(closed)capture.close();else postStatus("● RECORDING video only, up to 1 GiB. Stop to save locally; audio is not recorded.");
        }catch(Exception ex){if(capture!=null)capture.close();if(granted)try{remote.extension("recording-stop");}catch(Exception ignored){remote.close();}throw ex;}
    }
    private interface Job{void run() throws Exception;}
    private void run(Job job){
        if(closed || busy)return;busy=true;cancelled.set(false);enable(false);postStatus("Working…");
        try{worker.execute(() -> {try{job.run();}catch(Exception e){postStatus(cancelled.get()?"Cancelled; already committed files remain saved.":"Operation failed or was denied. Check the host owner permissions and connection.");}
            finally{main.post(() -> {busy=false;if(!closed)enable(true);});}});}catch(RejectedExecutionException e){busy=false;}
    }
    private void enable(boolean enabled){for(int i=0;i<actions.getChildCount();i++)actions.getChildAt(i).setEnabled(enabled);}
    private void postStatus(String value){main.post(() -> {if(!closed)status.setText(value);});}
    private void checkCancelled() throws InterruptedIOException{if(closed || cancelled.get() || Thread.currentThread().isInterrupted())throw new InterruptedIOException("Cancelled");}
    private static void checkText(String value) throws IOException{
        if(value.length()>1024 || value.indexOf('\0')>=0)throw new IOException("Invalid clipboard text");
        for(int i=0;i<value.length();i++){char c=value.charAt(i);if(Character.isHighSurrogate(c)){if(++i>=value.length() || !Character.isLowSurrogate(value.charAt(i)))throw new IOException("Invalid Unicode");}else if(Character.isLowSurrogate(c))throw new IOException("Invalid Unicode");}
    }
    private void pick(int request){
        if(closed || busy)return;
        run(() -> {stopAudio();main.post(() -> {
            if(closed)return;remote.release();picking=true;
            Intent intent=new Intent(request==UPLOAD?Intent.ACTION_OPEN_DOCUMENT:Intent.ACTION_OPEN_DOCUMENT_TREE);
            if(request==UPLOAD){intent.addCategory(Intent.CATEGORY_OPENABLE);intent.setType("*/*");}
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            try{activity.startActivityForResult(intent,request);}catch(RuntimeException e){picking=false;postStatus("No document picker is available");}
        });});
    }
    void picked(int request,int result,Intent data){
        picking=false;
        if(closed || result!=Activity.RESULT_OK || data==null || data.getData()==null){postStatus("File selection cancelled");return;}
        Uri uri=data.getData();if(request==UPLOAD)run(() -> upload(uri));else if(request==DOWNLOAD_FOLDER){String name=downloadName;run(() -> download(name,uri));}
    }
    private void listFiles(){run(() -> {
        List<String> names=new ArrayList<>();int offset=0;
        do{checkCancelled();JSONObject reply=remote.extension("file-list","offset",offset);JSONArray files=reply.getJSONArray("files");
            if(files.length()>8 || names.size()+files.length()>10000)throw new IOException("Invalid file listing");
            for(int i=0;i<files.length();i++){String name=files.getJSONObject(i).getString("name");checkName(name);names.add(name);}
            int next=reply.getInt("next");if(next!=-1 && next<=offset)throw new IOException("Invalid file listing cursor");offset=next;
        }while(offset!=-1);
        main.post(() -> {if(closed)return;if(names.isEmpty()){postStatus("The host's shared folder is empty");return;}
            new AlertDialog.Builder(activity).setTitle("Choose a host file").setItems(names.toArray(new String[0]),(d,which) -> {downloadName=names.get(which);pick(DOWNLOAD_FOLDER);}).setNegativeButton("Cancel",null).show();});
    });}
    private static void checkName(String name) throws IOException{
        if(name==null || name.length()<1 || name.length()>180 || name.equals(".") || name.equals("..") || name.matches(".*[\\\\/:*?\"<>|\\x00-\\x1f].*") || name.endsWith(".") || name.endsWith(" ") || name.toLowerCase(Locale.ROOT).startsWith(".hyperlink-"))throw new IOException("Unsafe file name");
    }
    private static String hex(byte[] bytes){StringBuilder value=new StringBuilder();for(byte b:bytes)value.append(String.format(Locale.ROOT,"%02x",b&255));return value.toString();}
    private static String hash(File file) throws Exception{MessageDigest digest=MessageDigest.getInstance("SHA-256");try(InputStream input=new FileInputStream(file)){byte[] buffer=new byte[65536];int n;while((n=input.read(buffer))!=-1)digest.update(buffer,0,n);}return hex(digest.digest());}
    private String documentName(Uri uri) throws IOException{
        try(Cursor cursor=activity.getContentResolver().query(uri,new String[]{OpenableColumns.DISPLAY_NAME},null,null,null)){
            if(cursor==null || !cursor.moveToFirst())throw new IOException("Missing document name");String name=cursor.getString(0);checkName(name);return name;
        }
    }
    private File stage() throws IOException{return File.createTempFile("hyperlink-transfer-",".part",activity.getCacheDir());}
    private void cancelHost(){try{remote.extension("file-cancel");}catch(Exception ignored){remote.close();}}
    private void upload(Uri source) throws Exception{
        String name=documentName(source);File staged=stage();boolean hostStarted=false;
        try{
            long length=0;try(InputStream input=activity.getContentResolver().openInputStream(source);FileOutputStream output=new FileOutputStream(staged)){
                if(input==null)throw new IOException("Document unavailable");byte[] buffer=new byte[65536];int n;
                while((n=input.read(buffer))!=-1){checkCancelled();length+=n;if(length>MAX_FILE)throw new IOException("File exceeds 1 GiB");output.write(buffer,0,n);}output.getFD().sync();
            }
            checkCancelled();String digest=hash(staged);
            hostStarted=true;JSONObject begin=remote.extension("file-upload-begin","name",name,"size",length,"sha256",digest);String id=begin.getString("id");
            try(InputStream input=new FileInputStream(staged)){byte[] buffer=new byte[8192];long offset=0;int n;
                while((n=input.read(buffer))!=-1){checkCancelled();String encoded=Base64.getEncoder().encodeToString(Arrays.copyOf(buffer,n));
                    JSONObject reply=remote.extension("file-upload-write","id",id,"offset",offset,"data",encoded);offset+=n;if(reply.getLong("offset")!=offset)throw new IOException("Invalid upload acknowledgement");}
            }
            checkCancelled();JSONObject commit=remote.extension("file-upload-commit","id",id);
            if(!commit.getBoolean("complete") || commit.getLong("size")!=length || !commit.getString("sha256").equals(digest))throw new IOException("Invalid upload acknowledgement");
            postStatus("Uploaded and verified: "+name);
        }finally{if(hostStarted)cancelHost();if(!staged.delete() && staged.exists())staged.deleteOnExit();}
    }
    private void download(String name,Uri tree) throws Exception{
        checkName(name);
        // Cloud providers may implement rename as copy/delete; require Android's local storage provider.
        if(!"com.android.externalstorage.documents".equals(tree.getAuthority()))throw new IOException("Choose a local Android storage folder");
        File staged=stage();Uri temporary=null;boolean published=false,hostStarted=false;
        try{
            ensureAbsent(tree,name);hostStarted=true;JSONObject begin=remote.extension("file-download-begin","name",name);String id=begin.getString("id");long size=begin.getLong("size");
            if(size<0 || size>MAX_FILE || activity.getCacheDir().getUsableSpace()<size+16*1024*1024)throw new IOException("Insufficient space or oversized file");
            MessageDigest digest=MessageDigest.getInstance("SHA-256");long offset=0;String expected=null;
            try(FileOutputStream output=new FileOutputStream(staged)){
                do{checkCancelled();JSONObject reply=remote.extension("file-download-read","id",id,"offset",offset);
                    String encoded=reply.getString("data");if(encoded.length()>10924)throw new IOException("Oversized file block");byte[] bytes=Base64.getDecoder().decode(encoded);
                    if(bytes.length>8192 || offset+bytes.length>size || reply.getLong("offset")!=offset+bytes.length)throw new IOException("Invalid file block");
                    output.write(bytes);digest.update(bytes);offset+=bytes.length;
                    if(reply.getBoolean("complete")){expected=reply.getString("sha256");break;}if(bytes.length==0)throw new IOException("Empty file block");
                }while(true);output.getFD().sync();
            }
            if(offset!=size || !hex(digest.digest()).equals(expected))throw new IOException("File integrity check failed");
            checkCancelled();Uri parent=DocumentsContract.buildDocumentUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));
            temporary=DocumentsContract.createDocument(activity.getContentResolver(),parent,"application/octet-stream",".hyperlink-"+UUID.randomUUID()+".part");
            if(temporary==null)throw new IOException("Could not stage destination");
            try(ParcelFileDescriptor descriptor=activity.getContentResolver().openFileDescriptor(temporary,"w");InputStream input=new FileInputStream(staged)){
                if(descriptor==null)throw new IOException("Destination unavailable");
                try(FileOutputStream output=new ParcelFileDescriptor.AutoCloseOutputStream(descriptor)){byte[] buffer=new byte[65536];int n;
                    while((n=input.read(buffer))!=-1){checkCancelled();output.write(buffer,0,n);}output.flush();output.getFD().sync();}
            }
            // Re-read the staged destination before making its final name visible.
            MessageDigest written=MessageDigest.getInstance("SHA-256");long writtenSize=0;
            try(InputStream input=activity.getContentResolver().openInputStream(temporary)){if(input==null)throw new IOException("Cannot verify destination");byte[] buffer=new byte[65536];int n;
                while((n=input.read(buffer))!=-1){checkCancelled();writtenSize+=n;if(writtenSize>size)throw new IOException("Destination length mismatch");written.update(buffer,0,n);}}
            if(writtenSize!=size || !hex(written.digest()).equals(expected))throw new IOException("Destination integrity check failed");
            checkCancelled();ensureAbsent(tree,name);
            Uri renamed=DocumentsContract.renameDocument(activity.getContentResolver(),temporary,name);if(renamed==null)throw new IOException("Destination cannot publish atomically");temporary=renamed;
            if(!documentName(renamed).equals(name))throw new IOException("Destination name changed");published=true;postStatus("Downloaded and verified: "+name);
        }finally{if(hostStarted)cancelHost();if(temporary!=null && !published)try{DocumentsContract.deleteDocument(activity.getContentResolver(),temporary);}catch(Exception ignored){}if(!staged.delete() && staged.exists())staged.deleteOnExit();}
    }
    private void ensureAbsent(Uri tree,String name) throws IOException{
        Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));
        try(Cursor cursor=activity.getContentResolver().query(children,new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME},null,null,null)){
            if(cursor==null)throw new IOException("Cannot inspect destination");while(cursor.moveToNext())if(name.equalsIgnoreCase(cursor.getString(0)))throw new IOException("A file with that name already exists");
        }
    }
    private void startAudio() throws Exception{
        if(audio!=null)throw new IOException("Audio is already active");
        boolean started=false;
        try{JSONObject format=remote.extension("audio-start");started=true;SessionAudio player=new SessionAudio(format);audio=player;remote.audioSink=player;
            if(closed){stopAudio();return;}postStatus("Playing host system audio · no microphone");
        }catch(Exception e){if(started)try{remote.extension("audio-stop");}catch(Exception ignored){remote.close();}throw e;}
    }
    private void stopAudio() throws Exception{
        remote.audioSink=null;SessionAudio player=audio;audio=null;if(player!=null){player.close();try{remote.extension("audio-stop");}catch(Exception e){remote.close();throw e;}}postStatus("Audio stopped");
    }
    public void close(){
        if(closed)return;closed=true;cancelled.set(true);remote.audioSink=null;SessionAudio player=audio;audio=null;if(player!=null)player.close();
        MjpegRecording closingRecording=recording;if(closingRecording!=null)closingRecording.close();
        // Queue cleanup after any active transfer; never block Android's UI thread.
        try{worker.execute(() -> {try{if(closingRecording!=null){remote.extension("recording-stop");closingRecording.completion.whenComplete((file,error) -> remote.recording.compareAndSet(closingRecording,null));}if(privacy){remote.extension("privacy-stop");privacy=false;}remote.extension("audio-stop");}catch(Exception ignored){remote.close();}cancelHost();});}catch(RejectedExecutionException ignored){remote.close();}worker.shutdown();
        if(dialog.isShowing())dialog.dismiss();
    }
}
