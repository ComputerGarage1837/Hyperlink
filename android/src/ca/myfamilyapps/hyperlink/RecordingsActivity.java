package ca.myfamilyapps.hyperlink;

import android.app.*;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.provider.DocumentsContract;
import android.view.WindowManager;
import android.widget.*;
import java.io.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;

public final class RecordingsActivity extends Activity {
    private static final int EXPORT=7401;
    private final ExecutorService worker=Executors.newSingleThreadExecutor(job -> {Thread thread=new Thread(job,"Hyperlink recording export");thread.setDaemon(true);return thread;});
    private final List<Button> buttons=new ArrayList<>();
    private LinearLayout body;private TextView status;private File pending;private boolean busy;private int offset;
    @Override public void onCreate(Bundle saved){
        super.onCreate(saved);getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        if(saved!=null){String name=saved.getString("pending");if(validName(name))pending=new File(folder(),name);}
        render();
    }
    private File folder(){return new File(getFilesDir(),"recordings");}
    private static boolean validName(String name){return name!=null && name.matches("Hyperlink-[0-9]{8}-[0-9]{6}-[a-f0-9]{8}\\.mkv");}
    private void render(){
        buttons.clear();ScrollView scroll=new ScrollView(this);body=new LinearLayout(this);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(24,24,24,24);body.setBackgroundColor(0xff0c1119);scroll.addView(body);setContentView(scroll);
        label("Recordings",24);label("Video clips are saved privately on this phone. Export to a local folder to open them in a video player. Audio is not included.",14);
        status=label("",14);button(body,"Back",this::finish);
        File[] files=folder().listFiles(file -> file.isFile() && validName(file.getName()));
        if(files==null || files.length==0){label("No recordings yet. Open Session tools during a connection and choose Record video on this phone.",16);return;}
        Arrays.sort(files,(a,b)->Long.compare(b.lastModified(),a.lastModified()));
        offset=Math.min(offset,((files.length-1)/200)*200);
        label("Showing "+(offset+1)+"–"+Math.min(files.length,offset+200)+" of "+files.length+" clips",14);
        if(offset>0)button(body,"Newer clips",()->{offset=Math.max(0,offset-200);render();});
        for(int i=offset;i<Math.min(files.length,offset+200);i++){
            final File file=files[i];label(file.getName()+"\n"+String.format(Locale.US,"%.1f MiB",file.length()/1048576.0),14);
            LinearLayout row=new LinearLayout(this);body.addView(row);
            button(row,"Export",() -> {if(busy)return;pending=file;Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION);try{startActivityForResult(intent,EXPORT);}catch(RuntimeException ex){pending=null;status.setText("No folder picker is available");}});
            button(row,"Delete",() -> {if(busy)return;new AlertDialog.Builder(this).setTitle("Delete this recording?").setMessage(file.getName()).setPositiveButton("Delete",(dialog,which)->{if(file.delete())render();else status.setText("Could not delete this recording");}).setNegativeButton("Cancel",null).show();});
        }
        if(offset+200<files.length)button(body,"Older clips",()->{offset+=200;render();});
    }
    private TextView label(String value,int size){TextView view=new TextView(this);view.setText(value);view.setTextSize(size);view.setTextColor(0xffe8eef7);view.setPadding(0,8,0,8);body.addView(view);return view;}
    private void button(LinearLayout row,String text,Runnable action){Button button=new Button(this);button.setText(text);button.setAllCaps(false);button.setOnClickListener(view->action.run());row.addView(button);buttons.add(button);}
    @Override protected void onSaveInstanceState(Bundle state){super.onSaveInstanceState(state);if(pending!=null)state.putString("pending",pending.getName());}
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);if(request!=EXPORT)return;File file=pending;pending=null;
        if(result!=RESULT_OK || data==null || data.getData()==null || file==null){status.setText("Export cancelled; your local clip is unchanged.");return;}
        busy=true;for(Button button:buttons)button.setEnabled(false);status.setText("Exporting and verifying…");Uri tree=data.getData();
        worker.execute(()->{String message;try{export(file,tree);message="Exported and verified. Your private original is still saved on this phone.";}catch(Exception ex){message="Export failed. Choose a local folder without an existing file of that name. Your private original is unchanged.";}
            final String completed=message;runOnUiThread(()->{if(isFinishing() || isDestroyed())return;busy=false;for(Button button:buttons)button.setEnabled(true);status.setText(completed);});});
    }
    private void export(File file,Uri tree) throws Exception {
        if(!validName(file.getName()) || !file.getCanonicalFile().getParentFile().equals(folder().getCanonicalFile()) || !file.isFile() || file.length()>1024L*1024*1024)throw new IOException("Invalid local recording");
        if(!"com.android.externalstorage.documents".equals(tree.getAuthority()))throw new IOException("Choose a local folder");
        Uri parent=DocumentsContract.buildDocumentUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));String name=file.getName();absent(tree,name);Uri temporary=null,renamed=null;
        try {
            temporary=DocumentsContract.createDocument(getContentResolver(),parent,"application/octet-stream",".hyperlink-"+UUID.randomUUID()+".part");if(temporary==null)throw new IOException("Could not stage export");
            MessageDigest digest=MessageDigest.getInstance("SHA-256");long length=0;
            ParcelFileDescriptor descriptor=getContentResolver().openFileDescriptor(temporary,"w");if(descriptor==null)throw new IOException("Destination unavailable");
            try(ParcelFileDescriptor.AutoCloseOutputStream output=new ParcelFileDescriptor.AutoCloseOutputStream(descriptor);InputStream input=new FileInputStream(file)){
                byte[] buffer=new byte[65536];int count;
                while((count=input.read(buffer))!=-1){checkCancelled();length+=count;if(length>1024L*1024*1024)throw new IOException("Recording limit");digest.update(buffer,0,count);output.write(buffer,0,count);}output.getFD().sync();
            }
            if(length!=file.length())throw new IOException("Recording changed during export");byte[] expected=digest.digest();
            if(!Arrays.equals(expected,hash(temporary)))throw new IOException("Export verification failed");absent(tree,name);checkCancelled();
            renamed=DocumentsContract.renameDocument(getContentResolver(),temporary,name);if(renamed==null || !name.equals(displayName(renamed)) || !Arrays.equals(expected,hash(renamed)))throw new IOException("Export finalization failed");
            temporary=null;renamed=null;
        }finally{if(renamed!=null)DocumentsContract.deleteDocument(getContentResolver(),renamed);else if(temporary!=null)DocumentsContract.deleteDocument(getContentResolver(),temporary);}
    }
    private void absent(Uri tree,String name) throws IOException {
        Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));int count=0;
        try(Cursor cursor=getContentResolver().query(children,new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME},null,null,null)){
            if(cursor==null)throw new IOException("Folder unavailable");while(cursor.moveToNext()){checkCancelled();if(++count>10000)throw new IOException("Folder too large");if(name.equals(cursor.getString(0)))throw new IOException("Existing files are not replaced");}
        }
    }
    private String displayName(Uri file) throws IOException {try(Cursor cursor=getContentResolver().query(file,new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME},null,null,null)){if(cursor==null || !cursor.moveToFirst())throw new IOException("Destination unavailable");return cursor.getString(0);}}
    private byte[] hash(Uri file) throws Exception {MessageDigest digest=MessageDigest.getInstance("SHA-256");try(InputStream input=getContentResolver().openInputStream(file)){if(input==null)throw new IOException("Destination unavailable");byte[] buffer=new byte[65536];long length=0;int count;while((count=input.read(buffer))!=-1){checkCancelled();length+=count;if(length>1024L*1024*1024)throw new IOException("Export limit");digest.update(buffer,0,count);}}return digest.digest();}
    private static void checkCancelled() throws InterruptedIOException {if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Export cancelled");}
    @Override public void onDestroy(){worker.shutdownNow();super.onDestroy();}
}
