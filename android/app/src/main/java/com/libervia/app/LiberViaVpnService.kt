package com.libervia.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.VpnService
import android.os.Build
import android.os.ParcelFileDescriptor
import android.service.quicksettings.TileService
import android.util.Log

class LiberViaVpnService : VpnService() {

    companion object {
        private const val TAG = "LiberViaVpnService"
        private const val CHANNEL_ID = "libervia_vpn_channel"
        private const val NOTIFICATION_ID = 1001

        const val DEFAULT_DPI_FLAGS = "-s 1 -d 1 -t 3"
        const val PRIMARY_DNS = "1.1.1.1"
        const val SECONDARY_DNS = "9.9.9.9"
        const val TUN_MTU = 1420

        const val ACTION_START = "com.libervia.app.ACTION_START"
        const val ACTION_STOP = "com.libervia.app.ACTION_STOP"
        const val ACTION_STATE_CHANGED = "com.libervia.app.ACTION_STATE_CHANGED"

        @Volatile
        var isRunning: Boolean = false
            private set

        init {
            System.loadLibrary("libervia_engine")
        }

        fun startServiceIntent(context: Context) {
            val intent = Intent(context, LiberViaVpnService::class.java).apply {
                action = ACTION_START
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stopServiceIntent(context: Context) {
            val intent = Intent(context, LiberViaVpnService::class.java).apply {
                action = ACTION_STOP
            }
            context.startService(intent)
        }
    }

    private var tunInterface: ParcelFileDescriptor? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopVpn()
            stopSelf()
            return START_NOT_STICKY
        }

        startVpn()
        return START_STICKY
    }

    private fun startVpn() {
        if (isRunning && tunInterface != null) {
            return
        }

        promoteToForeground()

        val builder = Builder()
            .setSession(getString(R.string.app_name))
            .setMtu(TUN_MTU)
            .addAddress("10.10.0.2", 24)
            .addRoute("0.0.0.0", 0)
            .addDnsServer(PRIMARY_DNS)
            .addDnsServer(SECONDARY_DNS)

        try {
            builder.addDisallowedApplication(packageName)
        } catch (_: Exception) {
        }

        val pfd = try {
            builder.establish()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to establish TUN interface", e)
            null
        }

        if (pfd == null) {
            stopForegroundCompat()
            broadcastState(false)
            stopSelf()
            return
        }

        tunInterface = pfd

        val rc = nativeStartEngine(pfd.fd, DEFAULT_DPI_FLAGS, PRIMARY_DNS)
        if (rc != 0) {
            Log.e(TAG, "nativeStartEngine failed with code $rc")
            stopVpn()
            stopSelf()
            return
        }

        isRunning = true
        broadcastState(true)
    }

    private fun stopVpn() {
        isRunning = false
        try {
            nativeStopEngine()
        } catch (e: Throwable) {
            Log.e(TAG, "nativeStopEngine error", e)
        }
        try {
            tunInterface?.close()
        } catch (_: Exception) {
        }
        tunInterface = null
        stopForegroundCompat()
        broadcastState(false)
    }

    override fun onRevoke() {
        stopVpn()
        stopSelf()
        super.onRevoke()
    }

    override fun onDestroy() {
        stopVpn()
        super.onDestroy()
    }

    fun protectSocket(fd: Int): Boolean {
        return protect(fd)
    }

    private external fun nativeStartEngine(tunFd: Int, flags: String, dnsServer: String): Int
    private external fun nativeStopEngine()
    external fun nativeGetStats(): LongArray

    private fun promoteToForeground() {
        ensureNotificationChannel()
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
    }

    private fun ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(NotificationManager::class.java) ?: return
            if (nm.getNotificationChannel(CHANNEL_ID) == null) {
                val channel = NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.notif_channel_name),
                    NotificationManager.IMPORTANCE_LOW
                ).apply {
                    setShowBadge(false)
                }
                nm.createNotificationChannel(channel)
            }
        }
    }

    private fun buildNotification(): Notification {
        val openIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
            },
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        val stopIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, LiberViaVpnService::class.java).apply {
                action = ACTION_STOP
            },
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }

        val action = Notification.Action.Builder(
            null,
            getString(R.string.notif_action_disengage),
            stopIntent
        ).build()

        return builder
            .setSmallIcon(R.drawable.ic_tile_libervia)
            .setContentTitle(getString(R.string.notif_title))
            .setContentText(getString(R.string.notif_text))
            .setContentIntent(openIntent)
            .setOngoing(true)
            .addAction(action)
            .build()
    }

    private fun broadcastState(running: Boolean) {
        val intent = Intent(ACTION_STATE_CHANGED).apply {
            setPackage(packageName)
            putExtra("running", running)
        }
        sendBroadcast(intent)

        try {
            TileService.requestListeningState(
                this,
                ComponentName(this, LiberViaTileService::class.java)
            )
        } catch (_: Exception) {
        }
    }
}
