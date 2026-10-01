package ca.myfamilyapps.hyperlink;
import android.app.*;
import android.content.*;
import android.content.pm.*;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.view.View;
import android.widget.*;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.concurrent.*;
final class AutoUpdates {
    private final Activity owner;private final Button install;private final TextView message;private final SharedPreferences prefs;
    private final ExecutorService worker=Executors.newSingleThreadExecutor();private volatile boolean closed;private boolean checking;private AutoUpdateFeed ready;private boolean requested;
    AutoUpdates(Activity activity,LinearLayout body) {
        owner=activity;prefs=owner.getSharedPreferences("updates",Context.MODE_PRIVATE);
        message=MainActivity.label(body,"",14,0xffa9bbcd);message.setVisibility(View.GONE);
        install=MainActivity.button(body,"Install downloaded update",this::install);install.setVisibility(View.GONE);
    }
    void check() {
        if(closed||checking)return;
        if(requested&&owner.getPackageManager().canRequestPackageInstalls()){requested=false;install();return;}
        checking=true;worker.execute(()->{
            try {
                String envelope=prefs.getString("feed","");AutoUpdateFeed feed=null;
                if(!envelope.isEmpty())try{feed=AutoUpdateFeed.verify(envelope);}catch(Exception ignored){}
                long now=System.currentTimeMillis();
                if(now-prefs.getLong("attempt",0)>=15*60*1000L&&now-prefs.getLong("checked",0)>=6*60*60*1000L) {
                    prefs.edit().putLong("attempt",now).apply();ByteArrayOutputStream bytes=new ByteArrayOutputStream();download("updates.json",bytes,16384,-1);
                    envelope=new String(bytes.toByteArray(),StandardCharsets.UTF_8);feed=AutoUpdateFeed.verify(envelope);prefs.edit().putString("feed",envelope).putLong("checked",now).apply();
                }
                if(feed==null||feed.code<=installedCode())return;
                File apk=file();if(!valid(apk,feed)) {
                    File temporary=new File(owner.getCacheDir(),"Hyperlink-update.partial");
                    try {try(FileOutputStream output=new FileOutputStream(temporary)){download("android.apk",output,feed.size,feed.size);output.getFD().sync();}
                        if(!valid(temporary,feed))throw new IOException("Update verification failed");
                        if(apk.exists()&&!apk.delete())throw new IOException("Could not replace cached update");if(!temporary.renameTo(apk))throw new IOException("Could not save update");
                    }finally{temporary.delete();}
                }
                final AutoUpdateFeed downloaded=feed;ui(()->{ready=downloaded;message.setText("Hyperlink "+downloaded.version+" downloaded. Android will ask you to approve installation.");message.setVisibility(View.VISIBLE);install.setVisibility(View.VISIBLE);});
            }catch(Exception ignored){}finally{ui(()->checking=false);}
        });
    }
    private int installedCode()throws Exception {return owner.getPackageManager().getPackageInfo(owner.getPackageName(),0).versionCode;}
    private File file(){return new File(owner.getCacheDir(),"Hyperlink-update.apk");}
    private boolean valid(File apk,AutoUpdateFeed feed)throws Exception {
        if(!apk.isFile()||apk.length()!=feed.size)return false;MessageDigest hash=MessageDigest.getInstance("SHA-256");
        try(InputStream input=new FileInputStream(apk)){byte[] buffer=new byte[32768];int n;while((n=input.read(buffer))>0)hash.update(buffer,0,n);}
        if(!Signing.hex(hash.digest()).equals(feed.hash))return false;
        int flags=Build.VERSION.SDK_INT>=28?PackageManager.GET_SIGNING_CERTIFICATES:PackageManager.GET_SIGNATURES;
        PackageInfo candidate=owner.getPackageManager().getPackageArchiveInfo(apk.getAbsolutePath(),flags),current=owner.getPackageManager().getPackageInfo(owner.getPackageName(),flags);
        if(candidate==null||!owner.getPackageName().equals(candidate.packageName)||candidate.versionCode!=feed.code||candidate.versionCode<=current.versionCode)return false;
        android.content.pm.Signature[] a=Build.VERSION.SDK_INT>=28?candidate.signingInfo.getApkContentsSigners():candidate.signatures,b=Build.VERSION.SDK_INT>=28?current.signingInfo.getApkContentsSigners():current.signatures;
        return a!=null&&b!=null&&a.length==1&&b.length==1&&Arrays.equals(a[0].toByteArray(),b[0].toByteArray());
    }
    private void install() {
        if(ready==null||closed)return;
        if(!owner.getPackageManager().canRequestPackageInstalls()) {requested=true;owner.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,Uri.parse("package:"+owner.getPackageName())));return;}
        install.setEnabled(false);final AutoUpdateFeed selected=ready;
        worker.execute(()->{int id=-1;boolean committed=false;PackageInstaller installer=owner.getPackageManager().getPackageInstaller();
            try {
                if(!valid(file(),selected))throw new IOException("Update no longer valid");
                PackageInstaller.SessionParams parameters=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);parameters.setAppPackageName(owner.getPackageName());parameters.setSize(selected.size);
                id=installer.createSession(parameters);
                try(PackageInstaller.Session session=installer.openSession(id)) {
                    try(InputStream input=new FileInputStream(file());OutputStream output=session.openWrite("base.apk",0,selected.size)) {byte[] buffer=new byte[32768];int count;while((count=input.read(buffer))>0)output.write(buffer,0,count);session.fsync(output);}
                    Intent callback=new Intent(owner,UpdateInstallReceiver.class).setAction("ca.myfamilyapps.hyperlink.INSTALL_RESULT");int flags=PendingIntent.FLAG_UPDATE_CURRENT|(Build.VERSION.SDK_INT>=31?PendingIntent.FLAG_MUTABLE:0);
                    session.commit(PendingIntent.getBroadcast(owner,id,callback,flags).getIntentSender());committed=true;
                }
            }catch(Exception error){ui(()->message.setText("Could not start installation. Tap Install to retry."));}
            finally{if(id>=0&&!committed)try{installer.abandonSession(id);}catch(Exception ignored){}ui(()->install.setEnabled(true));}
        });
    }
    private void download(String name,OutputStream output,long limit,long expected)throws Exception {
        HttpURLConnection connection=(HttpURLConnection)new URL("https://hyperlink.myfamilyapps.ca/"+name).openConnection();connection.setConnectTimeout(15000);connection.setReadTimeout(15000);connection.setInstanceFollowRedirects(false);connection.setRequestProperty("Accept-Encoding","identity");connection.setUseCaches(false);
        try {if(connection.getResponseCode()!=200||connection.getContentLengthLong()>limit||(expected>0&&connection.getContentLengthLong()!=expected))throw new IOException("Invalid update download");
            long total=0,start=System.nanoTime();try(InputStream input=connection.getInputStream()){byte[] buffer=new byte[32768];int n;while((n=input.read(buffer))>0){total+=n;if(closed||Thread.currentThread().isInterrupted()||total>limit||(System.nanoTime()-start)>120000000000L)throw new IOException("Download ended");output.write(buffer,0,n);}}
            if(expected>0&&total!=expected)throw new IOException("Incomplete update");
        }finally{connection.disconnect();}
    }
    private void ui(Runnable task){owner.runOnUiThread(()->{if(!closed&&!owner.isFinishing()&&!owner.isDestroyed())task.run();});}
    void close(){closed=true;worker.shutdownNow();}
}
