package com.mimic.client.capture

import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.util.Log
import com.mimic.client.capability.CapabilityBackend
import com.mimic.client.capability.CapabilityManager
import org.json.JSONObject

/**
 * Capture facade — MediaProjection (normal) / privileged VirtualDisplay (shizuku).
 */
class CaptureController(
    private val context: Context,
    private val caps: CapabilityManager,
) {
    private val tag = "MimicCap"
    @Volatile var streaming: Boolean = false
        private set
    @Volatile private var mediaProjectionResultCode: Int = 0
    @Volatile private var mediaProjectionData: Intent? = null
    private var projection: MediaProjection? = null
    private var encoder: ScreenEncoder? = null
    @Volatile private var vdMode: Boolean = false
    @Volatile var onEncodedFrame: ((ByteArray) -> Unit)? = null
    /** Fired when system revokes projection or encoder stops due to onStop. */
    @Volatile var onCaptureEnded: (() -> Unit)? = null

    fun setProjectionResult(resultCode: Int, data: Intent?) {
        mediaProjectionResultCode = resultCode
        mediaProjectionData = data?.let { Intent(it) }
    }

    fun hasProjectionConsent(): Boolean =
        mediaProjectionData != null && mediaProjectionResultCode != 0

    /** Consent Intent is one-shot on modern Android — clear after stop / revoke. */
    fun clearProjectionConsent() {
        mediaProjectionResultCode = 0
        mediaProjectionData = null
    }

    fun start(args: JSONObject, backend: CapabilityBackend): JSONObject {
        val targetId = args.optString("target_id", args.optString("id", "display:0"))
        val method = args.optString("method", "")
        val wantVd = method == "virtualdisplay" ||
            args.optBoolean("virtualDisplay", false) ||
            targetId.startsWith("app:")

        if (wantVd) {
            return startVirtualDisplay(args, targetId)
        }

        if (backend != CapabilityBackend.NORMAL && method.isNotBlank() && method != "mediaprojection") {
            return JSONObject()
                .put("ok", false)
                .put("error", "android: capture backend '${backend.id}' method='$method' not supported")
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
            // Tear down previous encoder/projection but keep CaptureService FGS when possible
            // (stop+restart races caused "foreground not ready" after MediaProjection consent).
            streaming = false
            try { encoder?.stop() } catch (_: Exception) {}
            encoder = null
            try { projection?.stop() } catch (_: Exception) {}
            projection = null

            if (!CaptureService.foregroundReady) {
                CaptureService.resetReady()
                val i = Intent(context, CaptureService::class.java)
                if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i)
                else context.startService(i)
                if (!CaptureService.awaitForeground(3500L)) {
                    Log.w(tag, "CaptureService not ready — retry start")
                    try { context.stopService(Intent(context, CaptureService::class.java)) } catch (_: Exception) {}
                    try { Thread.sleep(200) } catch (_: InterruptedException) {}
                    CaptureService.resetReady()
                    if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i)
                    else context.startService(i)
                    if (!CaptureService.awaitForeground(3500L)) {
                        Log.e(tag, "CaptureService foreground not ready in time")
                        return JSONObject()
                            .put("ok", false)
                            .put("error", "android: CaptureService foreground not ready")
                    }
                }
            }
            val mgr = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            val proj = mgr.getMediaProjection(mediaProjectionResultCode, mediaProjectionData!!)
                ?: return JSONObject().put("ok", false).put("error", "getMediaProjection returned null")
            clearProjectionConsent()
            projection = proj

            val dm = context.resources.displayMetrics
            Log.i(tag, "starting encoder ${dm.widthPixels}x${dm.heightPixels} target=$targetId")
            val enc = ScreenEncoder(
                width = dm.widthPixels,
                height = dm.heightPixels,
                dpi = dm.densityDpi,
                onFrame = { packed -> onEncodedFrame?.invoke(packed) },
                onProjectionStopped = {
                    Log.w(tag, "projection stopped by system")
                    stopInternal(clearConsent = true)
                    onCaptureEnded?.invoke()
                },
            )
            enc.start(proj)
            encoder = enc
            vdMode = false
            streaming = true
            JSONObject()
                .put("ok", true)
                .put("method", "mediaprojection")
                .put("target_id", targetId)
                .put("w", dm.widthPixels)
                .put("h", dm.heightPixels)
        } catch (e: Exception) {
            Log.e(tag, "capture start failed", e)
            stopInternal(clearConsent = true)
            JSONObject().put("ok", false).put("error", e.message ?: "capture start failed")
        }
    }

    private fun startVirtualDisplay(args: JSONObject, targetId: String): JSONObject {
        if (!targetId.startsWith("app:")) {
            return JSONObject()
                .put("ok", false)
                .put("error", "android: virtualdisplay requires app:* target")
                .put("target_id", targetId)
        }
        val conn = caps.ensureShizukuConnected()
        if (!conn.optBoolean("ok", false)) {
            return conn.put("target_id", targetId)
                .put("error", conn.optString("error", "android: Shizuku required for app sandbox"))
        }
        val rest = targetId.removePrefix("app:")
        val slash = rest.indexOf('/')
        val pkg = if (slash >= 0) rest.substring(0, slash) else rest
        val act = if (slash >= 0) rest.substring(slash + 1) else ""
        if (pkg.isBlank()) {
            return JSONObject().put("ok", false).put("error", "invalid app target id")
        }
        stopInternal(clearConsent = false)
        val dm = context.resources.displayMetrics
        val w = args.optInt("w", dm.widthPixels)
        val h = args.optInt("h", dm.heightPixels)
        val dpi = args.optInt("dpi", dm.densityDpi)
        val started = caps.shizuku.startAppSession(
            packageName = pkg,
            activity = act.ifBlank { null },
            width = w,
            height = h,
            dpi = dpi,
            onFrame = { packed -> onEncodedFrame?.invoke(packed) },
            onEnded = { reason ->
                Log.w(tag, "vd session ended: $reason")
                streaming = false
                vdMode = false
                onCaptureEnded?.invoke()
            },
        )
        if (!started.optBoolean("ok", false)) {
            return started.put("target_id", targetId)
        }
        vdMode = true
        streaming = true
        return started
            .put("ok", true)
            .put("method", "virtualdisplay")
            .put("target_id", targetId)
    }

    fun stop(): JSONObject {
        stopInternal(clearConsent = true)
        return JSONObject().put("ok", true)
    }

    fun requestKeyframe() {
        if (vdMode) caps.shizuku.requestKeyframe()
        else encoder?.requestKeyframe()
    }

    private fun stopInternal(clearConsent: Boolean) {
        streaming = false
        if (vdMode) {
            try { caps.shizuku.stopSession() } catch (_: Exception) {}
            vdMode = false
        }
        try { encoder?.stop() } catch (_: Exception) {}
        encoder = null
        try { projection?.stop() } catch (_: Exception) {}
        projection = null
        try { context.stopService(Intent(context, CaptureService::class.java)) } catch (_: Exception) {}
        if (clearConsent) clearProjectionConsent()
    }
}
