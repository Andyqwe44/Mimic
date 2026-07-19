package com.mimic.client.peer

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat

/**
 * Keeps Mimic alive while peer is logged in (signaling WS), even when WebView
 * is backgrounded. CaptureService still owns MediaProjection FGS separately.
 */
class PeerKeepAliveService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        ensureChannel()
        val n: Notification = NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle("Mimic")
            .setContentText("Peer online — keeping connection")
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(
                    NOTIF_ID,
                    n,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE,
                )
            } else if (Build.VERSION.SDK_INT >= 29) {
                @Suppress("DEPRECATION")
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
            } else {
                startForeground(NOTIF_ID, n)
            }
        } catch (e: Exception) {
            Log.e(TAG, "startForeground failed, retry plain", e)
            try {
                startForeground(NOTIF_ID, n)
            } catch (e2: Exception) {
                Log.e(TAG, "startForeground plain failed", e2)
            }
        }
        acquireWakeLock()
        return START_STICKY
    }

    override fun onDestroy() {
        releaseWakeLock()
        super.onDestroy()
    }

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mimic:peer").also {
                it.setReferenceCounted(false)
                it.acquire(6 * 60 * 60 * 1000L) // 6h cap; refreshed on restart
            }
        } catch (e: Exception) {
            Log.w(TAG, "wakeLock", e)
        }
    }

    private fun releaseWakeLock() {
        try {
            wakeLock?.let { if (it.isHeld) it.release() }
        } catch (_: Exception) {
        }
        wakeLock = null
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < 26) return
        val mgr = getSystemService(NotificationManager::class.java) ?: return
        mgr.createNotificationChannel(
            NotificationChannel(CHANNEL, "Peer", NotificationManager.IMPORTANCE_LOW),
        )
    }

    companion object {
        private const val TAG = "MimicPeerKeep"
        const val CHANNEL = "mimic_peer"
        const val NOTIF_ID = 43

        fun start(context: Context) {
            val i = Intent(context, PeerKeepAliveService::class.java)
            try {
                if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i)
                else context.startService(i)
            } catch (e: Exception) {
                Log.e(TAG, "start failed", e)
            }
        }

        fun stop(context: Context) {
            try {
                context.stopService(Intent(context, PeerKeepAliveService::class.java))
            } catch (_: Exception) {
            }
        }
    }
}
