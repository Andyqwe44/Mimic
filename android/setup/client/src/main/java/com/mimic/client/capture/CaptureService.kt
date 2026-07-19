package com.mimic.client.capture

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * Foreground service required for MediaProjection on modern Android.
 * Must call startForeground with MEDIA_PROJECTION type before getMediaProjection (API 34+).
 */
class CaptureService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        ensureChannel()
        val n: Notification = NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle("Mimic")
            .setContentText("Screen capture active")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setOngoing(true)
            .build()
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(
                    NOTIF_ID,
                    n,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION,
                )
            } else {
                startForeground(NOTIF_ID, n)
            }
            markForegroundReady()
            Log.i(TAG, "startForeground mediaProjection OK")
        } catch (e: Exception) {
            Log.e(TAG, "startForeground failed", e)
            // Still mark so waiter does not hang forever; getMediaProjection may fail next.
            markForegroundReady()
            throw e
        }
        return START_STICKY
    }

    override fun onDestroy() {
        // Do NOT clear foregroundReady here — stopService + restart races:
        // new instance can mark ready, then old onDestroy would wipe it and
        // awaitForeground times out ("CaptureService foreground not ready").
        super.onDestroy()
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < 26) return
        val mgr = getSystemService(NotificationManager::class.java) ?: return
        mgr.createNotificationChannel(
            NotificationChannel(CHANNEL, "Capture", NotificationManager.IMPORTANCE_LOW),
        )
    }

    companion object {
        private const val TAG = "MimicCapSvc"
        const val CHANNEL = "mimic_capture"
        const val NOTIF_ID = 42

        @Volatile var foregroundReady: Boolean = false
            private set
        private val readyLatch = AtomicReference<CountDownLatch?>(null)

        private fun markForegroundReady() {
            foregroundReady = true
            readyLatch.getAndSet(null)?.countDown()
        }

        /** Block until [startForeground] completed (or timeout). */
        fun awaitForeground(timeoutMs: Long = 2500L): Boolean {
            if (foregroundReady) return true
            val latch = CountDownLatch(1)
            readyLatch.set(latch)
            if (foregroundReady) {
                readyLatch.compareAndSet(latch, null)
                return true
            }
            return try {
                latch.await(timeoutMs, TimeUnit.MILLISECONDS) || foregroundReady
            } catch (_: InterruptedException) {
                false
            }
        }

        fun resetReady() {
            foregroundReady = false
        }
    }
}
