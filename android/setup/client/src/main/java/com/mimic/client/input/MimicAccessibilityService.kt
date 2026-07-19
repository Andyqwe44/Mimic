package com.mimic.client.input

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Intent
import android.graphics.Path
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.accessibility.AccessibilityEvent
import org.json.JSONObject
import java.lang.ref.WeakReference

/**
 * User-enabled AccessibilityService for normal-backend tap/swipe injection.
 * When [confinePackage] is set (app target), leaving that package triggers re-launch
 * so the controller cannot drive the user to Home / Recents / other apps.
 */
class MimicAccessibilityService : AccessibilityService() {
    private val main = Handler(Looper.getMainLooper())
    private var lastRelaunchMs = 0L
    private val confineWatch = object : Runnable {
        override fun run() {
            tryEnforceConfine("poll")
            if (confinePackage != null) main.postDelayed(this, 350L)
        }
    }

    override fun onServiceConnected() {
        instance = WeakReference(this)
        if (confinePackage != null) startConfineWatch()
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        if (confinePackage == null || event == null) return
        when (event.eventType) {
            AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED,
            AccessibilityEvent.TYPE_WINDOWS_CHANGED,
            AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED,
            -> tryEnforceConfine("event:${event.eventType} pkg=${event.packageName}")
        }
    }

    override fun onInterrupt() {}

    override fun onDestroy() {
        main.removeCallbacks(confineWatch)
        if (instance?.get() === this) instance = null
        super.onDestroy()
    }

    private fun startConfineWatch() {
        main.removeCallbacks(confineWatch)
        main.post(confineWatch)
    }

    private fun stopConfineWatch() {
        main.removeCallbacks(confineWatch)
    }

    /** If focused UI left the confined package (Home / Recents / other app), bounce back. */
    private fun tryEnforceConfine(reason: String) {
        val pkgConfine = confinePackage ?: return
        val focused = rootInActiveWindow?.packageName?.toString()
            ?: return
        if (focused == pkgConfine) return
        if (focused == ourPackage) return
        // Permission / installer sheets — allow briefly (user may need them).
        if (focused in PERMISSION_SHEETS) return
        // Home, Recents (systemui), launchers, other apps → re-launch confined target.
        val now = System.currentTimeMillis()
        if (now - lastRelaunchMs < 250) return
        lastRelaunchMs = now
        Log.i(TAG, "confine: left $pkgConfine → saw $focused ($reason); re-launch")
        val act = confineActivity
        main.post {
            relaunchConfined?.invoke(pkgConfine, act)
            // Nudge Recents away when possible (best-effort; gesture nav still needs relaunch).
            if (focused == "com.android.systemui" || isLauncherPackage(focused)) {
                try {
                    performGlobalAction(GLOBAL_ACTION_BACK)
                } catch (_: Exception) {
                }
            }
        }
    }

    private fun isLauncherPackage(pkg: String): Boolean {
        return try {
            val home = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME)
            val ri = packageManager.resolveActivity(home, 0) ?: return false
            ri.activityInfo?.packageName == pkg
        } catch (_: Exception) {
            false
        }
    }

    fun injectNormalized(action: JSONObject, screenW: Int, screenH: Int): JSONObject {
        val type = action.optString("type", "")
        val x = (action.optDouble("x_norm", action.optDouble("x", 0.5)).coerceIn(0.0, 1.0) * screenW).toFloat()
        val y = (action.optDouble("y_norm", action.optDouble("y", 0.5)).coerceIn(0.0, 1.0) * screenH).toFloat()
        return when (type) {
            "mousedown", "mouseup", "click", "tap" -> {
                if (Build.VERSION.SDK_INT < 24) {
                    return JSONObject().put("ok", false).put("error", "gesture API requires API 24+")
                }
                val path = Path().apply { moveTo(x, y) }
                val stroke = GestureDescription.StrokeDescription(path, 0, 50)
                val gesture = GestureDescription.Builder().addStroke(stroke).build()
                val ok = dispatchGesture(gesture, null, null)
                JSONObject().put("ok", ok).put("type", type)
                    .apply { if (!ok) put("error", "dispatchGesture failed") }
            }
            "move", "drag" -> {
                if (Build.VERSION.SDK_INT < 24) {
                    return JSONObject().put("ok", false).put("error", "gesture API requires API 24+")
                }
                val path = Path().apply { moveTo(x, y); lineTo(x, y) }
                val stroke = GestureDescription.StrokeDescription(path, 0, 30)
                val gesture = GestureDescription.Builder().addStroke(stroke).build()
                val ok = dispatchGesture(gesture, null, null)
                JSONObject().put("ok", ok).put("type", type)
            }
            "keydown", "keyup", "text" -> {
                JSONObject().put("ok", false).put("error", "android: key/text injection via a11y not implemented yet")
            }
            else -> JSONObject().put("ok", false).put("error", "unknown input type '$type'")
        }
    }

    companion object {
        private const val TAG = "MimicA11y"
        /** Transient system sheets only — NOT systemui Recents / launchers. */
        private val PERMISSION_SHEETS = setOf(
            "com.android.permissioncontroller",
            "com.google.android.permissioncontroller",
            "com.android.settings",
            "com.google.android.packageinstaller",
            "com.android.packageinstaller",
            "com.samsung.android.app.telephonyui",
            "com.android.phone",
        )

        @Volatile private var instance: WeakReference<MimicAccessibilityService>? = null
        @Volatile var confinePackage: String? = null
            private set
        @Volatile var confineActivity: String? = null
            private set
        @Volatile var ourPackage: String = ""
        @Volatile var relaunchConfined: ((pkg: String, activity: String?) -> Unit)? = null

        fun get(): MimicAccessibilityService? = instance?.get()

        fun isEnabled(): Boolean = get() != null

        fun setConfine(packageName: String?, activity: String?) {
            confinePackage = packageName?.ifBlank { null }
            confineActivity = activity?.ifBlank { null }
            Log.i(TAG, "confine=${confinePackage ?: "(none)"}")
            get()?.let { svc ->
                if (confinePackage != null) svc.startConfineWatch()
                else svc.stopConfineWatch()
            }
        }

        fun clearConfine() = setConfine(null, null)
    }
}
