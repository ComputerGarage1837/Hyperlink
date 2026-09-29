package com.hyperlink.app

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings as SystemSettings
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import kotlin.concurrent.thread

/** Updates the app from this project's GitHub releases. */
object Updater {
    data class Release(val version: String, val notes: String, val url: String, val size: Long, val sha256: String)

    private fun versionCode(v: String): Long {
        val parts = v.trimStart('v', 'V').split(".").map { p -> p.takeWhile { it.isDigit() }.toLongOrNull() ?: 0 }
        return parts.getOrElse(0) { 0 } * 1_000_000 + parts.getOrElse(1) { 0 } * 1000 + parts.getOrElse(2) { 0 }
    }

    fun isNewer(v: String) = versionCode(v) > versionCode(BuildConfig.VERSION_NAME)

    /** Blocking. Newest release that carries an APK. */
    fun latest(): Release {
        val c = URL("https://api.github.com/repos/${BuildConfig.GITHUB_REPO}/releases/latest")
            .openConnection() as HttpURLConnection
        c.setRequestProperty("Accept", "application/vnd.github+json")
        c.setRequestProperty("User-Agent", "Hyperlink-Android")
        c.connectTimeout = 10000
        c.readTimeout = 15000
        if (c.responseCode != 200) throw Exception("GitHub answered HTTP ${c.responseCode}")
        val o = JSONObject(c.inputStream.bufferedReader().readText())
        val assets = o.optJSONArray("assets")
        for (i in 0 until (assets?.length() ?: 0)) {
            val a = assets!!.getJSONObject(i)
            val name = a.optString("name")
            if (name.startsWith("Hyperlink-Android-") && name.endsWith(".apk")) {
                return Release(
                    version = o.optString("tag_name").trimStart('v', 'V'),
                    notes = o.optString("body"),
                    url = a.optString("browser_download_url"),
                    size = a.optLong("size"),
                    sha256 = a.optString("digest").removePrefix("sha256:").takeIf { it.length == 64 } ?: "",
                )
            }
        }
        throw Exception("The latest release has no Android app")
    }

    /**
     * Checks GitHub and, if there is a newer version, offers it. [userAsked] shows "up to date"
     * and errors; the automatic check on launch stays quiet unless there is something new.
     */
    fun check(activity: Activity, userAsked: Boolean) {
        val settings = Settings(activity)
        if (userAsked) Toast.makeText(activity, "Checking for updates…", Toast.LENGTH_SHORT).show()
        thread {
            val result = runCatching { latest() }
            activity.runOnUiThread {
                if (activity.isFinishing) return@runOnUiThread
                result.onFailure {
                    if (userAsked) MaterialAlertDialogBuilder(activity)
                        .setTitle("Update check failed").setMessage(it.message).setPositiveButton("OK", null).show()
                }
                result.onSuccess { r ->
                    when {
                        !isNewer(r.version) -> if (userAsked) MaterialAlertDialogBuilder(activity)
                            .setTitle("You're up to date")
                            .setMessage("Hyperlink ${BuildConfig.VERSION_NAME} is the latest version.")
                            .setPositiveButton("OK", null).show()
                        !userAsked && settings.skippedVersion == r.version -> Unit
                        else -> offer(activity, r)
                    }
                }
            }
        }
    }

    private fun offer(activity: Activity, r: Release) {
        MaterialAlertDialogBuilder(activity)
            .setTitle("Hyperlink ${r.version} is available")
            .setMessage("You have ${BuildConfig.VERSION_NAME}.\n\n${r.notes.take(2000)}")
            .setPositiveButton("Install") { _, _ -> download(activity, r) }
            .setNeutralButton("Skip this version") { _, _ -> Settings(activity).skippedVersion = r.version }
            .setNegativeButton("Later", null)
            .show()
    }

    private fun download(activity: Activity, r: Release) {
        if (Build.VERSION.SDK_INT >= 26 && !activity.packageManager.canRequestPackageInstalls()) {
            MaterialAlertDialogBuilder(activity)
                .setTitle("Allow updates")
                .setMessage("To install updates, allow Hyperlink to install apps. Then tap Install again.")
                .setPositiveButton("Open settings") { _, _ ->
                    activity.startActivity(
                        Intent(SystemSettings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${activity.packageName}"))
                    )
                }
                .setNegativeButton("Cancel", null).show()
            return
        }
        val bar = LinearProgressIndicator(activity).apply {
            max = 100
            isIndeterminate = r.size <= 0
            setPadding(64, 32, 64, 0)
        }
        val dialog: AlertDialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Downloading ${r.version}…").setView(bar).setCancelable(false).show()
        thread {
            val result = runCatching {
                val file = File(activity.cacheDir, "update.apk")
                val c = URL(r.url).openConnection() as HttpURLConnection
                c.setRequestProperty("User-Agent", "Hyperlink-Android")
                c.connectTimeout = 15000
                c.readTimeout = 30000
                if (c.responseCode != 200) throw Exception("Download failed: HTTP ${c.responseCode}")
                val total = if (r.size > 0) r.size else c.contentLengthLong
                val md = MessageDigest.getInstance("SHA-256")
                var done = 0L
                c.inputStream.use { input ->
                    file.outputStream().use { out ->
                        val buf = ByteArray(64 * 1024)
                        var lastPct = -1
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            md.update(buf, 0, n)
                            done += n
                            val pct = if (total > 0) (done * 100 / total).toInt() else 0
                            if (pct != lastPct) {
                                lastPct = pct
                                activity.runOnUiThread { bar.isIndeterminate = false; bar.progress = pct }
                            }
                        }
                    }
                }
                if (r.size > 0 && done != r.size) throw Exception("The download was incomplete")
                val hex = md.digest().joinToString("") { "%02x".format(it) }
                if (r.sha256.isNotEmpty() && hex != r.sha256) throw Exception("The download is damaged (checksum mismatch)")
                install(activity, file)
            }
            activity.runOnUiThread {
                dialog.dismiss()
                result.onFailure {
                    MaterialAlertDialogBuilder(activity).setTitle("Update failed").setMessage(it.message)
                        .setPositiveButton("OK", null).show()
                }
            }
        }
    }

    private fun install(ctx: Context, apk: File) {
        val installer = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(ctx.packageName)
        if (Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        val id = installer.createSession(params)
        installer.openSession(id).use { s ->
            s.openWrite("hyperlink.apk", 0, apk.length()).use { out ->
                apk.inputStream().use { it.copyTo(out) }
                s.fsync(out)
            }
            val intent = Intent(ctx, InstallReceiver::class.java)
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            s.commit(PendingIntent.getBroadcast(ctx, id, intent, flags).intentSender)
        }
    }
}

/** Receives PackageInstaller results; shows the system confirmation when Android asks for one. */
class InstallReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                @Suppress("DEPRECATION")
                val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT) ?: return
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                ctx.startActivity(confirm)
            }
            PackageInstaller.STATUS_SUCCESS -> Unit
            else -> {
                val msg = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "Unknown error"
                Toast.makeText(ctx, "Update not installed: $msg", Toast.LENGTH_LONG).show()
            }
        }
    }
}
