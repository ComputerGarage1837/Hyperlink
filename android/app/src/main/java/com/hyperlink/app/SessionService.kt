package com.hyperlink.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Keeps open connections alive while you use other apps: a small ongoing notification
 * ("Connected to …"). Tapping it returns to the session. Runs only while a session is open.
 */
class SessionService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val ids = ActiveSessions.openIds()
        if (ids.isEmpty()) {
            stopSelf()
            return START_NOT_STICKY
        }
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Active connections", NotificationManager.IMPORTANCE_LOW))
        }
        val names = ids.mapNotNull { ActiveSessions.names[it] }
        val open = Intent(this, MainActivity::class.java)
            .putExtra(MainActivity.EXTRA_RESUME, ids.last())
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        val pi = PendingIntent.getActivity(this, 1, open, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val n = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle(if (names.size == 1) "Connected to ${names[0]}" else "Connected to ${names.size} devices")
            .setContentText("Tap to return. Disconnect from the session's menu.")
            .setOngoing(true)
            .setContentIntent(pi)
            .build()
        if (Build.VERSION.SDK_INT >= 34) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        else startForeground(1, n)
        return START_NOT_STICKY
    }

    companion object {
        private const val CHANNEL = "sessions"

        /** Starts, refreshes or stops the service to match the open sessions. */
        fun update(ctx: Context) {
            val i = Intent(ctx, SessionService::class.java)
            if (ActiveSessions.openIds().isEmpty()) ctx.stopService(i)
            else runCatching { ctx.startForegroundService(i) }
        }
    }
}
