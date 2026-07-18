package com.mimic.client.capture

import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import com.mimic.client.capability.CapabilityBackend
import org.json.JSONObject

/**
 * Capture facade — MediaProjection (normal) / privileged VirtualDisplay (shizuku|root).
 */
class CaptureController(private val context: Context) {
    @Volatile var streaming: Boolean = false
        private set
    @Volatile private var mediaProjectionResultCode: Int = 0
    @Volatile private var mediaProjectionData: Intent? = null
    private var projection: MediaProjection? = null
    private var encoder: ScreenEncoder? = null
    @Volatile var onEncodedFrame: ((ByteArray) -> Unit)? = null

    fun setProjectionResult(resultCode: Int, data: Intent?) {
        mediaProjectionResultCode = resultCode
        mediaProjectionData = data?.let { Intent(it) }
    }

    fun hasProjectionConsent(): Boolean =
        mediaProjectionData != null && mediaProjectionResultCode != 0

    fun start(args: JSONObject, backend: CapabilityBackend): JSONObject {
        val targetId = args.optString("target_id", args.optString("id", "display:0"))
        val method = args.optString("method", "mediaprojection")
        if (backend != CapabilityBackend.NORMAL &&
            (method == "virtualdisplay" || args.optBoolean("virtualDisplay", false))
        ) {
            return JSONObject()
                .put("ok", false)
                .put("error", "android: virtual display requires ${backend.id} service (not ready)")
                .put("target_id", targetId)
        }
        if (backend != CapabilityBackend.NORMAL && method != "mediaprojection" && method.isNotBlank()) {
            return JSONObject()
                .put("ok", false)
                .put("error", "android: capture backend '${backend.id}' not implemented")
                .put("target_id", targetId)
        }
        if (!hasProjectionConsent()) {
            return JSONObject()
                .put("ok", false)
                .put("error", "android: MediaProjection consent required")
                .put("target_id", targetId)
                .put("need_consent", true)
        }
        return try {
            stopInternal()
            // FGS must be running before getMediaProjection on modern Android.
            val i = Intent(context, CaptureService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i)
            else context.startService(i)
            val mgr = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            val proj = mgr.getMediaProjection(mediaProjectionResultCode, mediaProjectionData!!)
                ?: return JSONObject().put("ok", false).put("error", "getMediaProjection returned null")
            projection = proj

            val dm = context.resources.displayMetrics
            val enc = ScreenEncoder(
                width = dm.widthPixels,
                height = dm.heightPixels,
                dpi = dm.densityDpi,
                onFrame = { packed -> onEncodedFrame?.invoke(packed) },
            )
            enc.start(proj)
            encoder = enc
            streaming = true
            JSONObject()
                .put("ok", true)
                .put("method", "mediaprojection")
                .put("target_id", targetId)
                .put("w", dm.widthPixels)
                .put("h", dm.heightPixels)
        } catch (e: Exception) {
            stopInternal()
            JSONObject().put("ok", false).put("error", e.message ?: "capture start failed")
        }
    }

    fun stop(): JSONObject {
        stopInternal()
        return JSONObject().put("ok", true)
    }

    fun requestKeyframe() {
        encoder?.requestKeyframe()
    }

    private fun stopInternal() {
        streaming = false
        try { encoder?.stop() } catch (_: Exception) {}
        encoder = null
        try { projection?.stop() } catch (_: Exception) {}
        projection = null
        try { context.stopService(Intent(context, CaptureService::class.java)) } catch (_: Exception) {}
    }
}
