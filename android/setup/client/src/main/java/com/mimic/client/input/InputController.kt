package com.mimic.client.input

import android.content.Context
import com.mimic.client.capability.CapabilityBackend
import com.mimic.client.capability.CapabilityManager
import org.json.JSONObject

/**
 * Input facade — Accessibility (normal) / privileged InputManager (shizuku VD).
 */
class InputController(
    private val context: Context,
    private val caps: CapabilityManager,
) {
    @Volatile var vdDisplayActive: Boolean = false

    fun inject(action: JSONObject, backend: CapabilityBackend): JSONObject {
        // Only app sandbox (active VD) uses privileged inject — not merely "Shizuku connected".
        if (vdDisplayActive) {
            if (!caps.shizuku.isConnected()) {
                return JSONObject()
                    .put("ok", false)
                    .put("error", "android: privileged inject requires Shizuku session")
            }
            return caps.shizuku.inject(action)
        }
        if (backend == CapabilityBackend.ROOT && caps.shizuku.isConnected()) {
            // Future root path may inject on default display; for now fall through to a11y.
        }
        val svc = MimicAccessibilityService.get()
            ?: return JSONObject()
                .put("ok", false)
                .put("error", "android: AccessibilityService not enabled")
        val dm = context.resources.displayMetrics
        return svc.injectNormalized(action, dm.widthPixels, dm.heightPixels)
    }
}
