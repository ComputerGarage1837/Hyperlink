package ca.myfamilyapps.hyperlink;
import android.content.*;
import android.content.pm.PackageInstaller;
import android.widget.Toast;
public final class UpdateInstallReceiver extends BroadcastReceiver {
    public void onReceive(Context context,Intent result) {
        int status=result.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE);
        if(status==PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirmation=result.getParcelableExtra(Intent.EXTRA_INTENT);
            if(confirmation!=null)try{context.startActivity(confirmation.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));}catch(Exception ignored){Toast.makeText(context,"Return to Hyperlink to install the update.",Toast.LENGTH_LONG).show();}
        }else if(status!=PackageInstaller.STATUS_SUCCESS)Toast.makeText(context,"Update was not installed. Your existing app and settings are unchanged.",Toast.LENGTH_LONG).show();
    }
}
